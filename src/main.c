#include <libspotify/api.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h>
#include <event2/keyvalq_struct.h>
#include <event2/thread.h>
#include <event2/util.h>

#include <stdlib.h>

// Application key
extern const unsigned char g_appkey[]; 
extern const size_t g_appkey_size; 

// Account information (in credentials.c)
extern const char username[];
extern const char password[];

static int exit_status = EXIT_FAILURE;

// Spotify account information
struct account {
  const char *username;
  const char *password;
};

struct state {
  sp_session *session;

  struct event_base *event_base;
  struct event *async;
  struct event *timer;
  struct event *sigint;
  struct timeval next_timeout;

  struct evhttp *http;
};


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

  // sp_playlistcontainer *pc = sp_session_playlistcontainer(session);
  // sp_playlistcontainer_add_callbacks(pc, &playlistcontainer_callbacks,
  //                                    session);
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



int main(int argc, char **argv) {

	printf("username: %s\n", username);
  struct account account = {
    .username = username,
    .password = password
  };

  // Initialize program state
  struct state *state = malloc(sizeof(struct state));

  // Initialize libev w/ pthreads
  evthread_use_pthreads();

  state->event_base = event_base_new();
  state->async = event_new(state->event_base, -1, 0, &process_events, state);
  state->timer = evtimer_new(state->event_base, &process_events, state);
  state->sigint = evsignal_new(state->event_base, SIGINT, &sigint_handler, state);

  // Initialize libspotify
  sp_session_callbacks session_callbacks = {
    .logged_in = &logged_in,
    .logged_out = &logged_out,
    .notify_main_thread = &notify_main_thread
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

  sp_session *session;
  sp_error session_create_error = sp_session_create(&session_config,
                                                    &session);

  if (session_create_error != SP_ERROR_OK)
    return EXIT_FAILURE;

  // Log in to Spotify
  printf("username: %s. password: %s\n", account.username, account.password);
  sp_session_login(session, account.username, account.password, 0);

  event_base_dispatch(state->event_base);

  event_free(state->async);
  event_free(state->timer);
  if (state->http != NULL) evhttp_free(state->http);
  event_base_free(state->event_base);
  free(state);
  return exit_status;

}
