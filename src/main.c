#include <libspotify/api.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <stdlib.h>
#include <signal.h>
#include <string.h>

#include "audio.h"


/// The output queue for audo data
static audio_fifo_t g_audiofifo;


// Application key
extern const unsigned char g_appkey[]; 
extern const size_t g_appkey_size; 

static int exit_status = EXIT_FAILURE;

// Spotify account information
struct account {
  const char *username;
  const char *password;
} account;

struct state {
  sp_session *session;

  struct event_base *event_base;
  struct event *async;
  struct event *timer;
  struct event *sigint;
  struct timeval next_timeout;

  struct evhttp *http;

  const char** urisToPlay;
  int nbUrisToPlay;

  sp_track *currentTrack;
  unsigned int currentTrackIdx;
  struct event *endOfTrack;
} *state;



// Catches SIGINT and exits gracefully
static void sigint_handler(evutil_socket_t socket,
                           short what,
                           void *userdata) {
  fprintf(stderr, "signal_handler\n");
  struct state *state = userdata;
  sp_session_logout(state->session);
}


static void logged_out(sp_session *session) {
  fprintf(stderr, "logged_out\n");
  struct state *state = sp_session_userdata(session);
  event_del(state->async);
  event_del(state->timer);
  event_del(state->sigint);
  event_base_loopbreak(state->event_base);
}


/*
 * Plays the track at index currentTrackIdx, stopping the current one if needed.
 */
static void playTrack() {
  // here we assume that everything about the current track (if any) that
  // had to be unloaded has been unloaded.

  if (NULL != state->currentTrack) {
    sp_session_player_unload(state->session);
    sp_track_release(state->currentTrack);
    state->currentTrack = NULL;
  }

  if (state->currentTrackIdx >= state->nbUrisToPlay) {
    fprintf(stderr, "No more uris to play\n");
    sp_session_logout(state->session);
    return ;
  }

  sp_link* l = sp_link_create_from_string(state->urisToPlay[state->currentTrackIdx]);
  // TODO add error check here
  state->currentTrack = sp_link_as_track(l);
  sp_track_add_ref(state->currentTrack);
  sp_link_release(l); l = NULL;

  if (sp_track_is_loaded(state->currentTrack))
  {
     fprintf(stderr, "track is loaded !\n");
  }
  else
  {
     fprintf(stderr, "track is not loaded :(\n");
  }

  fprintf(stderr, "OUT playNextTrack\n");

}

/**
 * Called in main thread after end_of_track has been called in the music delivery internal thread.
 */
static void process_end_of_track(evutil_socket_t socket,
                                 short what,
                                 void *userdata) {
  struct state *state = userdata;
  state->currentTrackIdx++;
  playTrack();
}


static void logged_in(sp_session *session, sp_error error) {
  if (error != SP_ERROR_OK) {
    fprintf(stderr, "%s\n", sp_error_message(error));
    exit_status = EXIT_FAILURE;
    logged_out(session);
    return;
  }

  struct state *state = sp_session_userdata(session);
  state->session = session;
  evsignal_add(state->sigint, NULL);

  playTrack();
}


static void process_events(evutil_socket_t socket,
                           short what,
                           void *userdata) {
  struct state *state = userdata;
  event_del(state->timer);
  int timeout = 0;

  do {
    sp_session_process_events(state->session, &timeout);
  } while (timeout == 0);

  state->next_timeout.tv_sec = timeout / 1000;
  state->next_timeout.tv_usec = (timeout % 1000) * 1000;
  evtimer_add(state->timer, &state->next_timeout);
}

static void notify_main_thread(sp_session *session) {
  fprintf(stderr, "notify_main_thread\n");
  struct state *state = sp_session_userdata(session);
  event_active(state->async, 0, 1);
}


static void metadata_updated(sp_session *session) {
	fprintf(stderr, "metadata updated.\n");
	if ((NULL != state->currentTrack) && sp_track_is_loaded (state->currentTrack))
	{
		fprintf(stderr, "track loaded. name: %s\n", sp_track_name(state->currentTrack));

		sp_error e = sp_session_player_load(session, state->currentTrack);
		if (e != SP_ERROR_OK) {
			fprintf(stderr, "error !\n");
			sp_track_release(state->currentTrack);
			state->currentTrack = NULL;
		}
		else {
			sp_session_player_play(session, 1);
		}
	}
	else {
		fprintf(stderr, "track not loaded yet\n");
	}
}


void end_of_track(sp_session *session) {
  fprintf(stderr, "end_of_track\n");
  struct state *state = sp_session_userdata(session);
  event_active(state->endOfTrack, 0, 1);
}


void start_playback(sp_session *session) {
	fprintf(stderr, "start playback callback\n");

}


void stop_playback(sp_session *session) {
	fprintf(stderr, "stop playback callback\n");
	
}


void get_audio_buffer_stats(sp_session *session, sp_audio_buffer_stats *stats)
{
//	fprintf(stderr, "get_audio_buffer_stats !\n");
}


static int music_delivery(sp_session *sess, const sp_audioformat *format,
                          const void *frames, int num_frames)
{
//	fprintf(stderr, "IN music delivery ! sample rate: %d, channels %d, %d frames\n", format->sample_rate, format->channels, num_frames);

  audio_fifo_t *af = &g_audiofifo;
  audio_fifo_data_t *afd;
  size_t s;

  if (num_frames == 0) {
    return 0; // Audio discontinuity, do nothing
  }

  pthread_mutex_lock(&af->mutex);

  /* Buffer one second of audio */
  if (af->qlen > format->sample_rate) {
    pthread_mutex_unlock(&af->mutex);
    return 0;
  }

  s = num_frames * sizeof(int16_t) * format->channels;

  afd = malloc(sizeof(audio_fifo_data_t) + s);
  memcpy(afd->samples, frames, s);

  afd->nsamples = num_frames;

  afd->rate = format->sample_rate;
  afd->channels = format->channels;

  TAILQ_INSERT_TAIL(&af->q, afd, link);
  af->qlen += num_frames;

  pthread_cond_signal(&af->cond);
  pthread_mutex_unlock(&af->mutex);

  return num_frames;
}

static void usage() {
  fprintf(stderr, "Usage: spotify_cmd <spotify_username> <spotify_password> <spotify_uri>\n");
}


static int parse_cmdline(int argc, const char **argv) {
  if (argc < 4) {
    usage();
    return 1;
  }
  account.username = argv[1];
  account.password = argv[2];
  state->nbUrisToPlay = argc - 3;
  state->urisToPlay = argv + 3;
  return 0;
}


int main(int argc, const char **argv) {

  // Initialize program state
  state = malloc(sizeof(struct state));

  if (parse_cmdline(argc, argv)) {
    return 1;
  }

  fprintf(stderr, "will play %s\n", state->urisToPlay[0]);

  // Initialize libev w/ pthreads
  evthread_use_pthreads();

  state->event_base = event_base_new();
  state->async = event_new(state->event_base, -1, 0, &process_events, state);
  state->timer = evtimer_new(state->event_base, &process_events, state);
  state->sigint = evsignal_new(state->event_base, SIGINT, &sigint_handler, state);

  state->endOfTrack = event_new(state->event_base, -1, 0, &process_end_of_track, state);
  state->currentTrack = NULL;
  state->currentTrackIdx = 0;

  // Initialize libspotify
  sp_session_callbacks session_callbacks = {
    .logged_in = &logged_in,
    .logged_out = &logged_out,
    .notify_main_thread = &notify_main_thread,
    .metadata_updated = &metadata_updated,
    .end_of_track = end_of_track,
    .start_playback = &start_playback,
    .stop_playback = &stop_playback,
    .get_audio_buffer_stats = &get_audio_buffer_stats,
    .music_delivery = &music_delivery
  };

  sp_session_config session_config = {
    .api_version = SPOTIFY_API_VERSION,
    .application_key = g_appkey,
    .application_key_size = g_appkey_size,
    .cache_location = ".cache",
    .callbacks = &session_callbacks,
    .compress_playlists = 0,
    .dont_save_metadata_for_playlists = 0,
    .settings_location = ".settings",
    .user_agent = "spotify_cmd",
    .userdata = state,
  };

  audio_init(&g_audiofifo);

  sp_session *session;
  sp_error session_create_error = sp_session_create(&session_config,
                                                    &session);

  if (session_create_error != SP_ERROR_OK)
    return EXIT_FAILURE;

  // Log in to Spotify
  printf("username: %s. password: %s\n", account.username, account.password);
  sp_session_login(session, account.username, account.password, 0);

  event_base_dispatch(state->event_base);

  event_free(state->endOfTrack);
  event_free(state->async);
  event_free(state->timer);
  if (state->http != NULL) evhttp_free(state->http);
  event_base_free(state->event_base);
  free(state);
  return exit_status;

}
