/* movie.h - the arcade opening movie (WMV3) decoder front-end.
 *
 * DirectShow is faked in dshow.c; the actual video frames come from the
 * runtime's ffmpeg, which already knows how to decode the WMV3/WMA stream.
 * A reader thread keeps the newest frame in memory so the game/renderer can
 * show it without blocking.
 */
#ifndef MOVIE_H
#define MOVIE_H

#include <stdint.h>

/* Start decoding `path` at width x height, 30 fps.  Returns 0 on success. */
int  movie_open(const char *path, int width, int height, int fps);

/* Stop the decoder (kills ffmpeg, joins the reader).  Safe to call twice. */
void movie_close(void);

/* 1 while the decoder is running (frames may still be pending). */
int  movie_active(void);

/* 1 once the stream ended and every frame was consumed. */
int  movie_finished(void);

/* Copy the newest BGRA frame into dst (dst_w x dst_h, scaled with a nearest
 * filter and alpha forced opaque).  Returns 1 when a new frame was written. */
int  movie_frame(uint8_t *dst, int dst_w, int dst_h);

/* Elapsed time of the newest frame, in seconds (0 when no frame yet). */
double movie_position(void);

#endif
