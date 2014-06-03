#include <iostream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <errno.h>
#include <unistd.h>
#include <cstring>
#include <netdb.h>
#include <ctime>

// Threads for handling the chat
#include <thread>
#include <mutex>

// For the list of moves
#include <vector>

// Reads the chat for input.
char* ircuser;
char* ircauth;
int conn;
char sbuf[512];

// The move queue
struct Movement {
  Movement(int _dx, int _dy) {
    dx = _dx;
    dy = _dy;
  }
  int dx, dy;
};
std::vector<Movement> moves;
std::mutex movesmutex;
int clearcounter = 0;

void raw(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sbuf, 512, fmt, ap);
    va_end(ap);
    printf("<< %s", sbuf);
    write(conn, sbuf, strlen(sbuf));
}

void parsechat(char* _message) {
  // No message, return
  if (_message == NULL)
    return;

  // This function will be locked whilst updating the screen.
  movesmutex.lock();

  // Figure out what kind of message it is.
  if (strstr(_message, "up") != 0)
    moves.push_back(Movement(0, -1));
  else if (strstr(_message, "down") != 0)
    moves.push_back(Movement(0, 1));
  else if (strstr(_message, "left") != 0)
    moves.push_back(Movement(-1, 0));
  else if (strstr(_message, "right") != 0)
    moves.push_back(Movement(1, 0));
  else if (strstr(_message, "clear") != 0)
    clearcounter += 1;

  // Unlock
  movesmutex.unlock();
}

int runchat() {
    char *nick = ircuser;
    char *channel = ircuser;
    char *auth = ircauth;
    char *host = "irc.twitch.tv";
    char *port = "6667";

    char *user, *command, *where, *message, *sep, *target;
    int i, j, l, sl, o = -1, start, wordcount;
    char buf[513];
    struct addrinfo hints, *res;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(host, port, &hints, &res);
    conn = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    connect(conn, res->ai_addr, res->ai_addrlen);

    raw("PASS oauth:%s\r\n", auth);
    raw("NICK %s\r\n", nick);
    raw("JOIN #%s\r\n", channel);
    raw("/viewers\r\n");

    while ((sl = read(conn, sbuf, 512))) {
        for (i = 0; i < sl; i++) {
            o++;
            buf[o] = sbuf[i];
            if ((i > 0 && sbuf[i] == '\n' && sbuf[i - 1] == '\r') || o == 512) {
                buf[o + 1] = '\0';
                l = o;
                o = -1;

                printf(">> %s", buf);

                if (!strncmp(buf, "PING", 4)) {
                    buf[1] = 'O';
                    raw(buf);
                } else if (buf[0] == ':') {
                    wordcount = 0;
                    user = command = where = message = NULL;
                    for (j = 1; j < l; j++) {
                        if (buf[j] == ' ') {
                            buf[j] = '\0';
                            wordcount++;
                            switch(wordcount) {
                                case 1: user = buf + 1; break;
                                case 2: command = buf + start; break;
                                case 3: where = buf + start; break;
                            }
                            if (j == l - 1) continue;
                            start = j + 1;
                        } else if (buf[j] == ':' && wordcount == 3) {
                            if (j < l - 1) message = buf + j + 1;
                            break;
                        }
                    }

                    if (wordcount < 2) continue;

                    if (!strncmp(command, "001", 3) && channel != NULL) {
                        raw("JOIN %s\r\n", channel);
                    } else if (!strncmp(command, "PRIVMSG", 7) || !strncmp(command, "NOTICE", 6)) {
                        if (where == NULL || message == NULL) continue;
                        if ((sep = strchr(user, '!')) != NULL) user[sep - user] = '\0';
                        if (where[0] == '#' || where[0] == '&' || where[0] == '+' || where[0] == '!') target = where; else target = user;
                        printf("[from: %s] [reply-with: %s] [where: %s] [reply-to: %s] %s", user, command, where, target, message);
                        //raw("%s %s :%s", command, target, message); // If you enable this the IRCd will get its "*** Looking up your hostname..." messages thrown back at it but it works...
                    }
                }

            }
        }

        // Take all of the messages.
        parsechat(message);
    }

    return 0;

}


// The screen
struct Pixel {
  Pixel() {}
  Pixel(unsigned char _r, unsigned char _g, unsigned char _b, unsigned char _a) {
    r = _r;
    g = _g;
    b = _b;
    a = _a;
  }
  unsigned char r, g, b, a;
};
Pixel* pixels;
Pixel* framepixels;
int width, height;
int framewidth, frameheight;
int pixelsize;

// The location of the cursor
int cursorx, cursory;
Pixel cursorColor;

// The generate the ffmpeg command for streaming.
FILE* GetFfmpegPipe(const char* _key, int _width, int _height, int _fps) {
  std::stringstream s;
  s << "ffmpeg -f rawvideo -pix_fmt rgba -s " << _width << "x" << _height << " "
    << "-r " << _fps << " -i - -f flv -vcodec libx264 -g " << _fps * 2 << " "
    << "-keyint_min " << _fps << " -b:v 1000k -minrate 1000k -maxrate 1000k -pix_fmt yuv420p "
    << "-s " << _width << "x" << _height << " -preset ultrafast -tune film "
    << "-threads 0 -strict normal -bufsize 1000k \"rtmp://live-ams.twitch.tv/app/" << _key << "\"";

  // Open up a pipe to ffmpeg.
  // (http://blog.mmacklin.com/2013/06/11/real-time-video-capture-with-ffmpeg/http://blog.mmacklin.com/2013/06/11/real-time-video-capture-with-ffmpeg/)
  FILE* ffmpeg = popen(s.str().c_str(), "w");
  if (ffmpeg == NULL) {
    std::cout << "Couldn't open FFMPEG. Error: " << errno << std::endl;
    return NULL;
  }

  return ffmpeg;
}

// Create the screen
void CreateScreen(int _framewidth, int _frameheight, int _width, int _height) {
  width = _width;
  height = _height;
  pixels = new Pixel[_width*_height];

  framewidth = _framewidth;
  frameheight = _frameheight;
  framepixels = new Pixel[_framewidth*_frameheight];


  // Clear the color to a white color
  for (int i = 0; i < _width*_height; ++i)
    pixels[i] = Pixel(255, 255, 255, 0);
}

Pixel* GetPixel(int _x, int _y) {
  return &pixels[_x + _y * width];
}

// Move the cursor
void MoveCursor(int _dx, int _dy) {
  // Check if it's possible
  int newx = cursorx + _dx;
  int newy = cursory + _dy;
  if (newx < 0 || newx >= width ||
      newy < 0 || newy >= height)
      return;

  // Change the pixel underneath the cursor to black
  *GetPixel(cursorx, cursory) = Pixel(0, 0, 0, 0);

  // Move the cursor
  cursorx = newx;
  cursory = newy;

  // Change the color of the cursor to red (to show where it is)
  *GetPixel(cursorx, cursory) = cursorColor;
}

// Do queued movement
void DoMovement() {
  // Lock
  movesmutex.lock();

  // Check if we should clear
  if (clearcounter > 10) {
    for (int i = 0; i < width*height; ++i)
      pixels[i] = Pixel(255, 255, 255, 0);

    *GetPixel(cursorx, cursory) = cursorColor;
    clearcounter = 0;
  }

  // Move all the queued up moves.
  for (int i = 0; i < moves.size(); ++i) {
    MoveCursor(moves[i].dx, moves[i].dy);
  }

  // Clear the moves
  moves.clear();

  // Unlock
  movesmutex.unlock();
}

// Encode a frame.
void WriteFrame(FILE* _ffmpeg) {
  // Scale up.
  float xratio = (float)width / (float)framewidth;
  float yratio = (float)height / (float)frameheight;
  for (int x = 0; x < framewidth; ++x) {
    for (int y = 0; y < frameheight; ++y) {
      framepixels[x + y * framewidth] = *GetPixel(x * xratio,
                                                  y * yratio);
    }
  }

  //memcpy(framepixels, pixels, sizeof(Pixel) * framewidth * frameheight);
  fwrite(framepixels, sizeof(Pixel) * framewidth * frameheight, 1, _ffmpeg);
  //fwrite(pixels, sizeof(Pixel) * width * height, 1, _ffmpeg);
}

// Main function.
int main(int _argc, char** _argv) {
  // Get the command line arguments
  if (_argc < 4) {
    std::cout << "TwitchPlaysEtchasketch [Twitch User] [IRC Auth key] [Stream key]" << std::endl;
    return 1;
  }

  // Create the screen.
  CreateScreen(1280, 720, 128, 72);

  // Set the cursor
  cursorx = width/2;
  cursory = height/2;
  cursorColor = Pixel(255, 0, 0, 0);
  *GetPixel(cursorx, cursory) = cursorColor;

  // Run the irc bot
  //ircuser = "bombpersonz";
  //ircauth = "p4qwplmi4ul73ve6cxlhz8huyytufwe";
  ircuser = _argv[1];
  ircauth = _argv[2];
  //"live_54987839_m5vBU41vOcS29EqRKBoLjjibdYTHL7"
  std::thread chatthread(runchat);

  // Start streaming =)
  float fps = 15;
  FILE* ffmpeg = GetFfmpegPipe(_argv[3],
                               framewidth, frameheight, fps);
  if (ffmpeg == NULL)
    return 1;

  // Loop forever
  clock_t oldticks = std::clock();
  clock_t ticks = 0;
  while (true) {
    // Update the timer
    ticks += std::clock() - oldticks;
    oldticks = std::clock();
    float timer = (float)ticks / CLOCKS_PER_SEC;

    // Get the input from chat. (locks the chat parser for a whilst we do this)
    DoMovement();

    // Write the frame if we have to.
    if (timer > 1.0f / fps) {
      ticks = 0;

      // Actually write the frame.
      std::cout << "Writing frame..." << std::endl;
      WriteFrame(ffmpeg);
    }
  }

  // Close the pipe
  pclose(ffmpeg);
}
