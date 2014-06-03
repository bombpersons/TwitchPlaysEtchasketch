TwitchPlaysEtchasketch
======================

Yet another TwitchPlays experiment thing. Dependencies - FFMPEG and a C++11 compiler.

Things you need.

1. A twitch username

2. An oauth token to log into irc (Look here for how to get one: http://www.twitchapps.com/tmi)

3. Your stream key

4. FFMPEG installed with rtmp and x264 support.

5. A C++11 compliant compiler to compile with.


Run build.sh script to build with G++.

To stream run the executable like this: 

twitchplaysetchasketch [Twitch User Name] [IRC oauth token (without the "oauth:" part)] [Stream key]
