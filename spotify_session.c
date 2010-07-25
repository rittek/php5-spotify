/**
 * A PHP extension to wrap some of libspotify
 * By Andrew Brampton <me@bramp.net> 2010
 *    for outsideline.co.uk
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "php_spotify.h"

#include <assert.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

// Loops waiting for a logged in event
// If one is not found after X seconds, a error is returned.
int wait_for_logged_in(php_spotify_session *session) {
	struct timespec ts;
	int err = 0;

	assert(session != NULL);

	session_lock(session);

	// Don't bother waiting if we already are logged in
	if (session_logged_in(session)) {
		session_unlock(session);
		return 0;
	}

	session->waiting_login++;

	session_unlock(session);

	DEBUG_PRINT("wait_for_logged_in (%d)\n", session->waiting_login);

	// Block for a max of 5 seconds
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += SPOTIFY_TIMEOUT;

	request_lock();

	while(err == 0) {
		int waiting;

		session_lock();
		waiting = session->waiting_login;
		session_unlock();

		// If we are finally logged in break.
		if (waiting == 0)
			break;

		// Wait until a callback is fired
		err = request_sleep_lock(&ts);
	}

	request_unlock();

	return err;
}

// Loops waiting for a logged out event
int wait_for_logged_out(php_spotify_session *session) {
	struct timespec ts;
	int err = 0;

	assert(session != NULL);

	session_lock(session);

	// Don't bother waiting if we already are logged in
	if (!session_logged_in(session)) {
		session_unlock(session);
		return 0;
	}

	session->waiting_logout++;

	session_unlock(session);

	DEBUG_PRINT("wait_for_logged_out\n");

	// Block for a max of 5 seconds
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += SPOTIFY_TIMEOUT;

	request_lock();

	while(err == 0) {
		int waiting;

		session_lock();
		waiting = session->waiting_logout;
		session_unlock();

		// If we are finally logged in break.
		if (waiting == 0)
			break;

		// Wait until a callback is fired
		err = request_sleep_lock(&ts);
	}

	request_unlock();

	DEBUG_PRINT("wait_for_logged_out end\n");

	return err;
}

/**
 * Session callbacks
 */
static void logged_in (sp_session *session, sp_error error) {
	php_spotify_session *resource = sp_session_userdata(session);
	assert(resource != NULL);

	DEBUG_PRINT("logged_in (%d)\n", error);

	session_lock(resource);

	resource->waiting_login = 0;

	if (error != SP_ERROR_OK)
		resource->last_error = error;

	session_unlock(resource);

	// Broadcast that we have now logged in (or not)
	request_wake();
}

static void logged_out (sp_session *session) {
	php_spotify_session *resource = sp_session_userdata(session);
	assert(resource != NULL);

	DEBUG_PRINT("logged_out\n");

	session_lock(resource);

	resource->waiting_logout = 0;

	session_unlock(resource);

	// Broadcast that we have now logged out
	request_wake();
}

static void metadata_updated (sp_session *session) {
	php_spotify_session *resource = sp_session_userdata(session);
	assert(resource != NULL);

	DEBUG_PRINT("metadata_updated\n");

	// Broadcast that we have some extra metadata
	request_wake();
}

static void connection_error (sp_session *session, sp_error error) {
	DEBUG_PRINT("connection_error\n");
}

static void message_to_user (sp_session *session, const char *message) {
	DEBUG_PRINT("message_to_user: %s", message);
}

static void notify_main_thread (sp_session *session) {
	php_spotify_session *resource = sp_session_userdata(session);
	assert(resource != NULL);

	DEBUG_PRINT("notify_main_thread\n");

	session_lock(resource);
	pthread_cond_signal(&session_cv);
	session_unlock(resource);
}

static void log_message (sp_session *session, const char *data) {
	DEBUG_PRINT("log_message: %s", data);
}

// Some callbacks we are not interested in, but written down for future's sake
//static int  music_delivery  (sp_session *session, const sp_audioformat *format, const void *frames, int num_frames);
//static void play_token_lost (sp_session *session);
//static void end_of_track    (sp_session *session);
//static void streaming_error (sp_session *session, sp_error error);
//static void userinfo_updated(sp_session *session);

static sp_session_callbacks callbacks = {
	&logged_in,
	&logged_out,
	&metadata_updated,
	&connection_error,
	&message_to_user,
	&notify_main_thread,
	NULL, //&music_delivery,
	NULL, //&play_token_lost,
	&log_message,
	NULL, //&end_of_track,
	NULL, //&streaming_error,
	NULL, //&userinfo_updated,
};

// Creates a session resource object
static php_spotify_session * session_resource_create(char *user) {
	php_spotify_session * resource;

	DEBUG_PRINT("session_resource_create\n");

	resource = pemalloc(sizeof(php_spotify_session), 1);
	resource->session       = NULL;
	resource->user          = pestrdup(user, 1);
	resource->events        = 0;
	resource->waiting_login = 0;
	resource->waiting_logout= 0;

	return resource;
}

static void session_resource_destory(php_spotify_session *resource) {

	if (resource == NULL)
		return;

	DEBUG_PRINT("session_resource_destory\n");

	/*
	pthread_mutex_destroy(&resource->mutex);
	pthread_cond_destroy (&resource->cv);
	*/

	pefree(resource->user, 1);
	pefree(resource, 1);
}

void php_spotify_session_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	php_spotify_session *session = (php_spotify_session*)rsrc->ptr;

    if (session) {
    	// Use reference counting and free the session
    	//zend_hash_add(&EG(persistent_list), PHP_SPOTIFY_SESSION_RES_NAME, strlen(PHP_SPOTIFY_SESSION_RES_NAME) + 1, &new_le, sizeof(list_entry), NULL);
    }
}

void php_spotify_session_p_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	php_spotify_session *session = (php_spotify_session*)rsrc->ptr;

	DEBUG_PRINT("php_spotify_session_p_dtor\n");

    if (session) {
    	int needsUnlock = 1;

        session_lock(session);

        // If we are logged in, log us out (and therefore save any remaining state).
        if (session_logged_in(session)) {

			sp_error error = sp_session_logout(session->session);

			if (error == SP_ERROR_OK) {
				// We don't need to hold the session lock when we enter the wait function.
				session_unlock(session);
				needsUnlock = 0;

				wait_for_logged_out(session);
			}
        }

        if (needsUnlock)
        	session_unlock(session);

        session_resource_destory(session);
    }

    DEBUG_PRINT("php_spotify_session_p_dtor end\n");
}


/* {{{ My own mkdir that checks if the dir exists and is a dir
 * Returns 0 on success, non-zero otherwise.
 */
static int _mkdir(const char *dir, mode_t mode) {
	struct stat st;

	// First check if it already exists
	if (stat(dir, &st)) {
		// If the path doesn't exist, lets try and make it
		if (errno == ENOENT) {
			if ( mkdir(dir, mode) )
				return -1;
		}
	}

	return !S_ISDIR(st.st_mode);
}

/* {{{ Recursively creates a directory */
static int mkdir_recursive(const char *dir, mode_t mode) {
	char *tmp, *p;
	size_t len;
	int err = -1;

	// Copy dir so we can change it
	tmp = estrdup(dir);
	len = strlen(tmp);

	// Trim trailing /
	if(tmp[len - 1] == '/')
		tmp[len - 1] = '\0';

	// Loop finding each / and mkdir it
	p = tmp;
	while ((p = strchr(p + 1, '/'))) {
		*p = '\0';
		err = _mkdir(tmp, mode);
		if (err) {
			goto cleanup;
		}
		*p = '/';
	}

	// Now try and make the full path
	err = _mkdir(tmp, mode);

cleanup:
	efree(tmp);
	return err;
}
/* }}} */


/* {{{ proto resource spotify_session_login(string username, string password, string appkey)
   Logs into spotify and returns a session resource on success
   TODO Ensure there is only one session at a time, and that this session is always for the same user
*/
PHP_FUNCTION(spotify_session_login) {
	sp_session_config config;
	sp_error error;

	php_spotify_session *resource = NULL;
	int err;
	//list_entry *le;
	list_entry new_le;

	char * cache_location;
	char * settings_location;

	char *user, *pass, *key;
	int user_len, pass_len, key_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sss",
		&user, &user_len,
		&pass, &pass_len,
		&key, &key_len) == FAILURE) {
		RETURN_FALSE;
	}

	if (user_len < 1) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "A blank username was given");
		RETURN_FALSE;
	}

	// Check if we already have a session
	session_lock(resource); // This works because currently the session lock is global. Later it will be inside the resource struct
	/*
	if (zend_hash_find(&EG(persistent_list), PHP_SPOTIFY_SESSION_RES_NAME, strlen(PHP_SPOTIFY_SESSION_RES_NAME) + 1, (void **)&le) == SUCCESS) {
		resource = le->ptr;
	*/
	if (session_resource != NULL) {
		resource = session_resource;
		if (strcmp(resource->user, user) != 0) {
			php_error_docref(NULL TSRMLS_CC, E_ERROR, "Only one session can be created per process. There is already a session for \"%s\"", resource->user);
			session_unlock(resource);
			RETURN_FALSE;
		}
		// Now jump down and ensure we have logged in, etc
		if (!session_logged_in(resource))
			goto login;
		goto done;
	}

	if (pass_len < 1) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "A blank password was given");
		RETURN_FALSE;
	}

	if (key_len < 1) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "A blank appkey was given");
		RETURN_FALSE;
	}

	// Use the username in the cache dir
	//spprintf(&cache_location,    0, "/tmp/libspotify-php/%s/cache/",    user);
	//spprintf(&settings_location, 0, "/tmp/libspotify-php/%s/settings/", user);

	// To be multiple process safe, use the PID
	spprintf(&cache_location,    0, "/tmp/libspotify-php/%d/cache/",    getpid());
	spprintf(&settings_location, 0, "/tmp/libspotify-php/%d/settings/", getpid());

	// A bug in libspotify means we should create the directories
	if ( mkdir_recursive(cache_location, S_IRWXU) ) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Failed to create cache directory \"%s\"", cache_location);
		goto error;
	}

	if ( mkdir_recursive(settings_location, S_IRWXU) ) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Failed to create settings directory \"%s\"", settings_location);
		goto error;
	}

	resource = session_resource_create(user);
	if (resource == NULL) {
		// A error message will be printed in session_resource_create()
		RETURN_FALSE;
	}

	config.api_version       = SPOTIFY_API_VERSION;
	config.cache_location    = cache_location;
	config.settings_location = settings_location;

	config.application_key      = key;
	config.application_key_size = key_len;

	config.user_agent = "libspotify for PHP";
	config.callbacks = &callbacks;

	// Pass the resource to libspotify as userdata
	config.userdata = resource;

	// Create the session
	error = sp_session_init(&config, &resource->session);

	// Ensure we always free these strings
	efree(cache_location);
	efree(settings_location);

	if (error != SP_ERROR_OK) {
		SPOTIFY_G(last_error) = error;
		goto error;
	}

	// Set the global session and signal for the main_thread to start
	session_resource = resource;
	session          = resource->session;
	pthread_cond_signal(&session_cv);

login:
	DEBUG_PRINT("sp_session_login\n");

	// Now try and log in
	error = sp_session_login(resource->session, user, pass);
	if (error != SP_ERROR_OK) {
		SPOTIFY_G(last_error) = error;
		goto error;
	}

	session_unlock(resource);

	err = wait_for_logged_in(resource);
	if (err == ETIMEDOUT) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "Timeout while logging in.");
		goto error;
	}

	session_lock(resource);

done:
	if (!session_logged_in(resource)) {
		SPOTIFY_G(last_error) = resource->last_error;
		goto error;
	}

	session_unlock(resource);

	ZEND_REGISTER_RESOURCE(return_value, resource, le_spotify_session);

	/* Store a reference in the persistence list */
	new_le.ptr  = resource;
	new_le.type = le_spotify_session;
	zend_hash_add(&EG(persistent_list), PHP_SPOTIFY_SESSION_RES_NAME, strlen(PHP_SPOTIFY_SESSION_RES_NAME) + 1, &new_le, sizeof(list_entry), NULL);

	return;

error:
	// TODO In the future libspotify will add sp_session_release. Call that here.
	session_unlock(resource);
	session_resource_destory(resource);
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool spotify_session_logout(resource session)
   Logs out of spotify and returns true on success */
PHP_FUNCTION(spotify_session_logout) {

    php_spotify_session *session;
    zval *zsession;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zsession) == FAILURE) {
        RETURN_FALSE;
    }

    // Check its a spotify session (otherwise RETURN_FALSE)
    ZEND_FETCH_RESOURCE(session, php_spotify_session*, &zsession, -1, PHP_SPOTIFY_SESSION_RES_NAME, le_spotify_session);

    session_lock(session);

	sp_error error = sp_session_logout(session->session);
	if (error != SP_ERROR_OK) {
		SPOTIFY_G(last_error) = error;
		session_unlock(session);
		RETURN_FALSE;
	}

	session_unlock(session);

	// Wait for logout
	wait_for_logged_out(session);

	// Now invalidate the resource
	zend_list_delete(Z_LVAL_P(zsession));

	// TODO In the future libspotify will add sp_session_release. Call that here.

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int spotify_session_connectionstate(resource session)
   Returns the state the session is in. */
PHP_FUNCTION(spotify_session_connectionstate) {
    php_spotify_session *session;
    zval *zsession;
    sp_connectionstate state;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zsession) == FAILURE) {
        RETURN_FALSE;
    }

    // Check its a spotify session (otherwise RETURN_FALSE)
    ZEND_FETCH_RESOURCE(session, php_spotify_session*, &zsession, -1, PHP_SPOTIFY_SESSION_RES_NAME, le_spotify_session);

    session_lock(session);
    state = sp_session_connectionstate(session->session);
    session_unlock(session);

	RETURN_LONG( state );
}
/* }}} */

/* {{{ proto string spotify_session_user(resource session)
   Returns the currently logged in user */
PHP_FUNCTION(spotify_session_user) {
    php_spotify_session *session;
    zval *zsession;
    sp_user * user;
    const char *name;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zsession) == FAILURE) {
        RETURN_FALSE;
    }

    // Check its a spotify session (otherwise RETURN_FALSE)
    ZEND_FETCH_RESOURCE(session, php_spotify_session*, &zsession, -1, PHP_SPOTIFY_SESSION_RES_NAME, le_spotify_session);

    session_lock(session);
    user = sp_session_user(session->session);
    if (user == NULL) {
        php_error_docref(NULL TSRMLS_CC, E_ERROR, "The session is not logged in.");
        session_unlock(session);
        RETURN_FALSE;
    }

    name = sp_user_canonical_name(user);
    session_unlock(session);

    RETURN_STRING(name, 1);
}
/* }}} */
