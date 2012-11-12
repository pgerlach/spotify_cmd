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
#include <fcntl.h>

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
  struct event *ev_stdin;

  struct evhttp *http;

  sp_track *currentTrack;
  int currentTrackPlaying;
  unsigned int currentTrackIdx;
  struct event *endOfTrack;

  const char **urisToPlay;
  int nbUrisToPlay;

  sp_track **tracklist;
  unsigned int tracklistLen;
  int tracklistSomethingLoading;
  int tracklistLoadingIdx;
  sp_albumbrowse *tracklistCurrentlyLoadingAlbumBrowse;
  sp_playlist *tracklistCurrentlyLoadingPlaylist;
  sp_track *tracklistCurrentlyLoadingTrack;

  sp_playlist_callbacks *playlistCallbacks;
} *state;


static void playTrack(struct state *state);
static void tracklistFill(struct state *state);

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


static void stdin_setup(struct state *state) {
  int flags = fcntl(fileno(stdin), F_GETFL, 0);
  fprintf(stderr, "flags: %d. stdin non blocking ? %d\n", flags, !!(flags & O_NONBLOCK));
  flags |= O_NONBLOCK;
  int err = fcntl(fileno(stdin), F_SETFL, flags);
  fprintf(stderr, "res: %d\n", err);

  event_add(state->ev_stdin, NULL);
}


static void stdin_data(evutil_socket_t socket,
                       short what,
                       void *userdata) {
  struct state *state = userdata;
  char c;
  static char buf[256];
  while (EOF != (c = fgetc(stdin))) {
    ungetc(c, stdin);
    fgets(buf, 256, stdin);
    fprintf(stderr, "line on stdin: %s\n", buf);
    if (!strcmp(buf, "next\n")) {
      fprintf(stderr, "going to next track\n");
      state->currentTrackIdx++;
      if (state->currentTrackIdx == state->tracklistLen) {
        state->currentTrackIdx = 0; // loop
      }
      playTrack(state);
    }
    else if (!strcmp(buf, "prev\n")) {
      fprintf(stderr, "going to previous track\n");
      if (state->currentTrackIdx > 0) {
        state->currentTrackIdx--;
      }
      else {
        state->currentTrackIdx = state->tracklistLen-1;
      }
      playTrack(state);
    } else if (!strcmp(buf, "stop\n")) {
      sp_session_logout(state->session);
    }
    else {
      fprintf(stderr, "unknown command \"%s\"", buf);
    }
  }
  
}


/**
  * Really starts the playing of the current track (assumes it is fully loaded)
  */
static void launchPlayCurrentTrack(struct state* state) {
  fprintf(stderr, "launchPlayCurrentTrack, idx %d\n", state->currentTrackIdx);
  sp_error e = sp_session_player_load(state->session, state->currentTrack);
  if (e != SP_ERROR_OK) {
    fprintf(stderr, "error while launching current track: %s\n", sp_error_message(sp_track_error(state->currentTrack)));
    // TODO investigate causes !
    sp_track_release(state->currentTrack);
    state->currentTrack = NULL;
    state->currentTrackIdx++;
    playTrack(state);
  }
  else {
    state->currentTrackPlaying = 1;
    sp_session_player_play(state->session, 1);
  }
}



/*
 * Plays the track at index currentTrackIdx, stopping the current one if needed.
 */
static void playTrack(struct state *state) {
  // here we assume that everything about the current track (if any) that
  // had to be unloaded has been unloaded.


  fprintf(stderr, "playtrack. state->currentTrackIdx=%d\n", state->currentTrackIdx);

  if (NULL != state->currentTrack) {
    sp_session_player_unload(state->session);
    sp_track_release(state->currentTrack);
    state->currentTrack = NULL;
    state->currentTrackPlaying = 0;
  }

  if (state->currentTrackIdx >= state->tracklistLen) {
    fprintf(stderr, "No more tracks to play\n");
    sp_session_logout(state->session);
    return ;
  }

  state->currentTrack = state->tracklist[state->currentTrackIdx];
  sp_track_add_ref(state->currentTrack);

  if (sp_track_is_loaded(state->currentTrack))
  {
     fprintf(stderr, "track is loaded !\n");
     launchPlayCurrentTrack(state);
  }
  else
  {
     fprintf(stderr, "track is not loaded :(\n");
  }
}


/**
 * Called when tracklist is full and we can begin playing music
 */
static void letsPlay(struct state *state) {
  stdin_setup(state);

  fprintf(stderr, "Will now begin playback. %d tracks in tracklist\n", state->tracklistLen);
  for (int i=0; i<state->tracklistLen; ++i) {
    sp_track *t = state->tracklist[i];
    fprintf(stderr, " [%d] \"%s\" (\"%s\" // \"%s\")\n", i, sp_track_name(t), sp_album_name(sp_track_album(t)), sp_artist_name(sp_album_artist(sp_track_album(t))));
    fflush(stderr);
  }
  state->currentTrackIdx = 0;
  playTrack(state);
}

/**
 * Called in main thread after end_of_track has been called in the music delivery internal thread.
 */
static void process_end_of_track(evutil_socket_t socket,
                                 short what,
                                 void *userdata) {
  struct state *state = userdata;
  state->currentTrackIdx++;
  playTrack(state);
}


static void tracklistAddTrack(struct state* state, sp_track* track) {
  sp_track_add_ref(track);
  if (!sp_track_is_loaded(track)) {
    fprintf(stderr, "Trying to add a track not loaded yet.\n");
    // can only happen if adding a single track. When adding tracks from
    // playlists or albums, they have been loaded.
    state->tracklistSomethingLoading = 1;
    state->tracklistCurrentlyLoadingTrack = track;
  }
  else {
    fprintf(stderr, "Adding track \"%s\" to tracklist\n", sp_track_name(track));

    if (SP_TRACK_AVAILABILITY_AVAILABLE == sp_track_get_availability (state->session, track))
    {
      state->tracklistLen++;
      state->tracklist = realloc(state->tracklist, state->tracklistLen * sizeof(sp_track*));
      state->tracklist[state->tracklistLen-1] = track;
    }
    else {
      fprintf(stderr, "Track %s not available\n", sp_track_name(track));
      sp_track_release(track);
    }
  }
}



/**
 * Callback called when album information has been loaded. If it has been, then all tracks have been.
 */
void trackListAddAlbumAlbumBrowseCb(sp_albumbrowse *result, void *userdata) {
  struct state *state = userdata;
  if (result != state->tracklistCurrentlyLoadingAlbumBrowse) {
    fprintf(stderr, "ERR: result != state->currentlyLoadingAlbumBrowse");
    return ;
  }
  fprintf(stderr, "%d tracks in the album \"%s\"\n", sp_albumbrowse_num_tracks(result), sp_album_name(sp_albumbrowse_album(result)));
  for (int i=0; i < sp_albumbrowse_num_tracks(result); ++i) {
    tracklistAddTrack(state, sp_albumbrowse_track(result, i));
  }
  sp_albumbrowse_release(state->tracklistCurrentlyLoadingAlbumBrowse);
  state->tracklistCurrentlyLoadingAlbumBrowse = NULL;

  state->tracklistSomethingLoading = 0;
  (state->tracklistLoadingIdx)++;
  tracklistFill(state);
}


static void tracklistAddAlbum(struct state* state, sp_album* album) {
  sp_albumbrowse *albumBrowse = sp_albumbrowse_create(state->session, album, &trackListAddAlbumAlbumBrowseCb, state);
  state->tracklistCurrentlyLoadingAlbumBrowse = albumBrowse;
  state->tracklistSomethingLoading = 1;
}


/**
 * Assumes playlist is loaded. If it is, we know all tracks are.
 */
static void tracklistDoAddPlaylist(struct state* state, sp_playlist *pl) {
  for (int i=0; i<sp_playlist_num_tracks(pl); ++i) {
    tracklistAddTrack(state, sp_playlist_track(pl, i));
  }
}


/**
 * Callback used when loading playlists
 */
static void playlist_metadata_updated(sp_playlist *pl, void *userdata) {
  fprintf(stderr, "playlist metadata updated\n");
  if (pl != state->tracklistCurrentlyLoadingPlaylist) {
      fprintf(stderr, "ERR: pl != state->currentlyLoadingPlaylist\n");
      // TODO something (skip, I guess)
      return ;
  }
  if (sp_playlist_is_loaded(pl)) {
    // wait until all the tracks of the playlist are loaded
    for (int i=0; i<sp_playlist_num_tracks(pl); ++i) {
      if (!sp_track_is_loaded(sp_playlist_track(pl, i))) {
        fprintf(stderr, "not all tracks of playlist are loaded. wait.\n");
        return ;
      }
    }
    fprintf(stderr, "playlist is loaded, and all of its tracks.\n");
    tracklistDoAddPlaylist(state, pl);
    sp_playlist_remove_callbacks(pl, state->playlistCallbacks, state);
    sp_playlist_release(state->tracklistCurrentlyLoadingPlaylist);
    state->tracklistCurrentlyLoadingPlaylist = NULL;
    state->tracklistSomethingLoading = 0;
  }
  else {
    fprintf(stderr, "playlist still not loaded\n");
  }
}


static void tracklistAddPlaylist(struct state* state, sp_link* playlistLink) {
  sp_playlist* pl = sp_playlist_create(state->session, playlistLink);
  if (sp_playlist_is_loaded(pl)) {
    tracklistDoAddPlaylist(state, pl);
  }
  else {
    sp_playlist_add_callbacks (pl, state->playlistCallbacks, state);
    state->tracklistCurrentlyLoadingPlaylist = pl;
    state->tracklistSomethingLoading = 1;
  }
}


/**
 * Loads a uri and add the associated tracks into the tracklist.
 */
static void tracklistAddUri(struct state* state, const char* uri) {
  fprintf(stderr, "add uri \"%s\"\n", uri);

  sp_link* l = sp_link_create_from_string(uri);
  if (NULL == l) {
    fprintf(stderr, "Could not parse uri \"%s\", skipping\n", uri);
    return ;
  }
  switch (sp_link_type(l)) {
    case SP_LINKTYPE_TRACK:
      tracklistAddTrack(state, sp_link_as_track(l));
      break;
    case SP_LINKTYPE_ALBUM:
      tracklistAddAlbum(state, sp_link_as_album(l));
      break;
    case SP_LINKTYPE_PLAYLIST:
      tracklistAddPlaylist(state, l);
      break;
    default:
      fprintf(stderr, "Unhandled link type \"%s\", skipping\n", uri);
      break;
  }
  sp_link_release(l);
  l = NULL;
}


/*
 * For each arg, if it is a track, add it to the tracklist, if it is an album or a playlist,
 * add all its tracks to the tracklist.
 * Before the first call, state->tracklistLoadingIdx should be 0.
 */
static void tracklistFill(struct state *state) {
  fprintf(stderr, "trackListFill\n");
  for ( ; state->tracklistLoadingIdx < state->nbUrisToPlay; state->tracklistLoadingIdx++) {
    tracklistAddUri(state, state->urisToPlay[state->tracklistLoadingIdx]);
    if (state->tracklistSomethingLoading)
      // asynchronous load
      break ;
  }

  if (!state->tracklistSomethingLoading) {
    letsPlay(state);
  }
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

  tracklistFill(state);
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
//  fprintf(stderr, "notify_main_thread\n");
  struct state *state = sp_session_userdata(session);
  event_active(state->async, 0, 1);
}


static void metadata_updated(sp_session *session) {
  struct state *state = sp_session_userdata(session);
	fprintf(stderr, "metadata updated.\n");
  if (NULL == state->currentTrack) {
    // we're still populating tracklist
    if (NULL != state->tracklistCurrentlyLoadingTrack) {
        if (sp_track_is_loaded(state->tracklistCurrentlyLoadingTrack)) {
        tracklistAddTrack(state, state->tracklistCurrentlyLoadingTrack);
        state->tracklistCurrentlyLoadingTrack = NULL;
        state->tracklistSomethingLoading = 0;
        state->tracklistLoadingIdx++;
        tracklistFill(state);
      }
      else {
        fprintf(stderr, "track not loaded yet\n");
      }
    }
    else {
      // other reason, we don't care
    }
  }
  else {
  	if (sp_track_is_loaded (state->currentTrack) && !state->currentTrackPlaying)
    {
      fprintf(stderr, "track loaded. name: %s\n", sp_track_name(state->currentTrack));
      launchPlayCurrentTrack(state);
    }
    else {
		  fprintf(stderr, "track not loaded yet\n");
    }
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
  state->ev_stdin = event_new(state->event_base, fileno(stdin), EV_READ|EV_PERSIST, &stdin_data, state);

  state->endOfTrack = event_new(state->event_base, -1, 0, &process_end_of_track, state);
  state->currentTrack = NULL;
  state->currentTrackPlaying = 0;
  state->currentTrackIdx = 0;

  state->tracklist = NULL;
  state->tracklistSomethingLoading = 0;
  state->tracklistLoadingIdx = 0;
  state->tracklistCurrentlyLoadingAlbumBrowse = NULL;

  sp_playlist_callbacks playlist_callbacks = {
    .playlist_metadata_updated = playlist_metadata_updated,
  };
  state->playlistCallbacks = &playlist_callbacks;

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
  sp_session_login(session, account.username, account.password, 0, NULL);

  event_base_dispatch(state->event_base);

  event_free(state->endOfTrack);
  event_free(state->async);
  event_free(state->timer);
  if (state->http != NULL) evhttp_free(state->http);
  event_base_free(state->event_base);
  free(state);
  return exit_status;

}
