/* Copyright (C) 2026 Sandro Miisamm.  GPL-3.0-or-later, see LICENSE. */
/* slimplay.c: SlimProto PCM player core (see there) */
int slimplay_start(const char *server, const char *name, const unsigned char mac[6]);
void slimplay_info(int *connected, int *state, char *msg, int msglen);
void slimplay_stop(void);                /* before exit */
int slimplay_format(int *rate);          /* format of the stream now playing: 'p' pcm, 'm' mp3, 0 none */
