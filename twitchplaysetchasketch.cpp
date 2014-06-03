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
#include <chrono>

// Threads for handling the chat
#include <thread>
#include <mutex>

// For the list of moves
#include <vector>

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
int width, height;
int framewidth, frameheight;
int pixelsize;

std::mutex framemutex;
int clearcounter = 0;

// The location of the cursor
int cursorx, cursory;
Pixel cursorColor;

Pixel* GetPixel(int _x, int _y) {
  return &pixels[_x + _y * width];
}

// The generate the ffmpeg command for streaming.
FILE* GetFfmpegPipe(const char* _key, int _fps) {
  std::stringstream s;
  s << "ffmpeg -f rawvideo -pix_fmt rgba -s " << width << "x" << height
    << " -r " << _fps << " -i - -f flv -vcodec libx264 -g " << _fps * 2
    << " -keyint_min " << _fps << " -b:v 1000k -minrate 1000k -maxrate 1000k -pix_fmt yuv420p"
    << " -s " << framewidth << "x" << frameheight << " -sws_flags neighbor -preset ultrafast -tune film"
    << " -threads 0 -strict normal -bufsize 1000k \"rtmp://live-ams.twitch.tv/app/" << _key << "\"";

  // Open up a pipe to ffmpeg.
  // (http://blog.mmacklin.com/2013/06/11/real-time-video-capture-with-ffmpeg/http://blog.mmacklin.com/2013/06/11/real-time-video-capture-with-ffmpeg/)
  FILE* ffmpeg = popen(s.str().c_str(), "w");
  if (ffmpeg == NULL) {
    std::cout << "Couldn't open FFMPEG. Error: " << errno << std::endl;
    return NULL;
  }

  return ffmpeg;
}

// Clear the screen
void ClearScreen() {
  std::lock_guard<std::mutex> lock(framemutex);
  for (int i = 0; i < width*height; ++i)
    pixels[i] = Pixel(255, 255, 255, 0);

  *GetPixel(cursorx, cursory) = cursorColor;
  clearcounter = 0;
}

// Create the screen
void CreateScreen(int _framewidth, int _frameheight, int _width, int _height) {
  width = _width;
  height = _height;
  pixels = new Pixel[_width*_height];
  framewidth = _framewidth;
  frameheight = _frameheight;

  cursorx = width/2;
  cursory = height/2;

  // Clear the color to a white color
  ClearScreen();
}

// Move the cursor
void MoveCursor(int _dx, int _dy) {
  // Check if it's possible
  int newx = cursorx + _dx;
  int newy = cursory + _dy;
  if (newx < 0 || newx >= width ||
      newy < 0 || newy >= height)
      return;

  // Lock
  std::lock_guard<std::mutex> lock(framemutex);

  // Moving
  std::cout << "Moving cursor by " << _dx << ", " << _dy << std::endl;

  // Change the pixel underneath the cursor to black
  *GetPixel(cursorx, cursory) = Pixel(0, 0, 0, 0);

  // Move the cursor
  cursorx = newx;
  cursory = newy;

  // Change the color of the cursor to red (to show where it is)
  *GetPixel(cursorx, cursory) = cursorColor;
}

// Encode a frame.
void WriteFrame(FILE* _ffmpeg) {
  std::lock_guard<std::mutex> lock(framemutex);
  fwrite(pixels, sizeof(Pixel) * width * height, 1, _ffmpeg);
}

// Reads the chat for input.
char* ircuser;
char* ircauth;
int conn;
char sbuf[512];

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

  // Figure out what kind of message it is.
  if (strstr(_message, "up") != 0)
    MoveCursor(0, -1);
  else if (strstr(_message, "down") != 0)
    MoveCursor(0, 1);
  else if (strstr(_message, "left") != 0)
    MoveCursor(-1, 0);
  else if (strstr(_message, "right") != 0)
    MoveCursor(1, 0);
  else if (strstr(_message, "clear") != 0) {
    clearcounter += 1;
    if (clearcounter > 10)
      ClearScreen();
  }
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

// Main function.
int main(int _argc, char** _argv) {
  // Get the command line arguments
  if (_argc < 4) {
    std::cout << "TwitchPlaysEtchasketch [Twitch User] [IRC Auth key] [Stream key]" << std::endl;
    return 1;
  }

  // Cursor color
  cursorColor = Pixel(255, 0, 0, 0);

  // Create the screen.
  CreateScreen(1280, 720, 128, 72);

  // Run the irc bot
  ircuser = _argv[1];
  ircauth = _argv[2];
  std::thread chatthread(runchat);

  // Start streaming =)
  float fps = 24;
  FILE* ffmpeg = GetFfmpegPipe(_argv[3], fps);
  if (ffmpeg == NULL)
    return 1;

  // Loop forever
  typedef std::chrono::high_resolution_clock Clock;
  auto oldticks = Clock::now();
  while (true) {
    //float timer = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - oldticks).count() / 1000000000.0f;
    std::this_thread::sleep_for(std::chrono::nanoseconds((long int)(1000000000 * (1.0f / fps))));

    // Write frames at the right timing.
    if (true) {
      oldticks = Clock::now();

      //MoveCursor(-1 + rand() % 3, -1 + rand() % 3);

      // Actually write the frame.
      //std::cout << "Writing Frame..." << std::endl;
      WriteFrame(ffmpeg);
    }
  }

  // Close the pipe
  pclose(ffmpeg);
}
