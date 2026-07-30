// Data source listeners
// ---- MIME type matching helper ----
// Returns true if requested mime matches the stored mime,
// handling common text MIME type equivalences.
static bool mime_text_match(const char *requested, const char *stored)
{
	if(!requested || !stored)
		return false;
	if(strcmp(requested, stored) == 0)
		return true;
	// text/plain and UTF8_STRING are interchangeable for clipboard text
	if(strcmp(stored, "text/plain") == 0 &&
	   strcmp(requested, "UTF8_STRING") == 0)
		return true;
	if(strcmp(stored, "UTF8_STRING") == 0 &&
	   strcmp(requested, "text/plain") == 0)
		return true;
	return false;
}

static void data_control_source_send(void *data,
	struct ext_data_control_source_v1 *ext_data_control_source_v1,
	const char *mime_type,
	int32_t fd)
{
	// Check if requested MIME type matches what we have
	if(!wl_data_control_source_data.mime || !wl_data_control_source_data.data ||
	   wl_data_control_source_data.size <= 0 ||
	   !mime_text_match(mime_type, wl_data_control_source_data.mime))
	{
		if(wl_debug)
			fprintf(stderr, "[wui-wayland] data control source send: unsupported mime '%s'\n", mime_type);
		close(fd);
		return;
	}

	// Write data to compositor's fd
	ssize_t written = 0;
	ssize_t total = 0;
	while(total < wl_data_control_source_data.size)
	{
		written = write(fd, wl_data_control_source_data.data + total,
					wl_data_control_source_data.size - total);
		if(written <= 0)
			break;
		total += written;
	}
	close(fd);

	if(wl_debug)
		fprintf(stderr, "[wui-wayland] data control source send: %s, wrote %zd bytes\n",
			mime_type, total);
}

static void data_control_source_cancelled(void *data,
	struct ext_data_control_source_v1 *ext_data_control_source_v1)
{
	if(wl_debug)
		fprintf(stderr, "[wui-wayland] data control source cancelled\n");

	// Clean up the source — compositor no longer holds it
	if(ext_data_control_source_v1 == wl_active_source)
		wl_active_source = NULL;
	ext_data_control_source_v1_destroy(ext_data_control_source_v1);

	// No longer own the selection
	wl_owns_clipboard = false;
}

static const struct ext_data_control_source_v1_listener data_control_source_listener = {
	.send = data_control_source_send,
	.cancelled = data_control_source_cancelled,
};

// Registry listeners
static void data_control_device_selection(void *data,
	struct ext_data_control_device_v1 *ext_data_control_device_v1,
	struct ext_data_control_offer_v1 *id)
{
	if(wl_data_control_offer)
		ext_data_control_offer_v1_destroy(wl_data_control_offer);
	wl_data_control_offer = id;

	// Non-NULL offer means another client's data
	if(id != NULL)
		wl_owns_clipboard = false;

	if(wl_debug)
		fprintf(stderr, "[wui-wayland] data control selection changed: %p\n", (void*)id);
}

static void data_control_device_finished(void *data,
	struct ext_data_control_device_v1 *ext_data_control_device_v1)
{
	if(wl_debug)
		fprintf(stderr, "[wui-wayland] data control device finished\n");
}

static void data_control_device_data_offer(void *data,
			   struct ext_data_control_device_v1 *ext_data_control_device_v1,
			   struct ext_data_control_offer_v1 *id)
{
	if(wl_debug)
		fprintf(stderr,"[wui-wayland] data control device data offer\n");
}

static void data_control_device_primary_selection(void *data,
				  struct ext_data_control_device_v1 *ext_data_control_device_v1,
				  struct ext_data_control_offer_v1 *id)
{
	if(wl_debug)
		fprintf(stderr,"[wui-wayland] data control device primary selection\n");
}

static const struct ext_data_control_device_v1_listener data_control_device_listener = {
	.data_offer = data_control_device_data_offer,
	.selection = data_control_device_selection,
	.finished = data_control_device_finished,
	.primary_selection = data_control_device_primary_selection,
};

// ---- Standard clipboard (wl_data_device) fallback ----

static void data_device_data_offer(void *data,
	struct wl_data_device *device, struct wl_data_offer *id)
{
}

static void data_device_enter(void *data,
	struct wl_data_device *device, uint32_t serial,
	struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
	struct wl_data_offer *id)
{
	// DND enter — not needed
	wl_data_offer_destroy(id);
}

static void data_device_leave(void *data,
	struct wl_data_device *device)
{
}

static void data_device_motion(void *data,
	struct wl_data_device *device, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
}

static void data_device_drop(void *data,
	struct wl_data_device *device)
{
}

static void data_device_selection_std(void *data,
	struct wl_data_device *device, struct wl_data_offer *id)
{
	// Track clipboard selection
	if(wl_data_offer)
		wl_data_offer_destroy(wl_data_offer);
	wl_data_offer = id;

	if(wl_debug)
		fprintf(stderr, "[wui-wayland] std clipboard selection changed: %p\n", (void*)id);
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.enter = data_device_enter,
	.leave = data_device_leave,
	.motion = data_device_motion,
	.drop = data_device_drop,
	.selection = data_device_selection_std,
};

// wl_data_source listener for standard clipboard write
static void data_source_send_std(void *data,
	struct wl_data_source *source, const char *mime_type, int32_t fd)
{
	// Same data matching logic as ext-data-control version
	if(!wl_data_control_source_data.mime || !wl_data_control_source_data.data ||
	   wl_data_control_source_data.size <= 0 ||
	   !mime_text_match(mime_type, wl_data_control_source_data.mime))
	{
		if(wl_debug)
			fprintf(stderr, "[wui-wayland] std source send: unsupported mime '%s'\n", mime_type);
		close(fd);
		return;
	}

	ssize_t total = 0;
	while(total < wl_data_control_source_data.size)
	{
		ssize_t written = write(fd, wl_data_control_source_data.data + total,
				wl_data_control_source_data.size - total);
		if(written <= 0)
			break;
		total += written;
	}
	close(fd);

	if(wl_debug)
		fprintf(stderr, "[wui-wayland] std source send: %s, wrote %zd bytes\n", mime_type, total);
}

static void data_source_cancelled_std(void *data,
	struct wl_data_source *source)
{
	if(wl_debug)
		fprintf(stderr, "[wui-wayland] std data source cancelled\n");

	if(source == wl_data_source_active)
		wl_data_source_active = NULL;
	wl_data_source_destroy(source);

	// No longer own the selection
	wl_owns_clipboard = false;
}

static const struct wl_data_source_listener data_source_listener_std = {
	.send = data_source_send_std,
	.cancelled = data_source_cancelled_std,
};

// ---- w_clipboard_get_text ----
// Try ext-data-control first, then standard wl_data_device as fallback.
// Returns a newly allocated string (caller must free), or NULL on failure.
char *w_clipboard_get_text(void)
{
	// Fast path: if we own the clipboard, return data from memory directly
	if(wl_owns_clipboard &&
	   wl_data_control_source_data.data &&
	   wl_data_control_source_data.size > 0 &&
	   wl_data_control_source_data.mime)
	{
		if(mime_text_match(wl_data_control_source_data.mime, "text/plain"))
			return l_memdup0(wl_data_control_source_data.data,wl_data_control_source_data.size);
		return NULL;
	}

	// Path 1: ext-data-control (preferred — no serial/focus requirement)
	if(wl_data_control_offer && wl_data_control_device)
	{
		int pipefd[2];
		if(pipe(pipefd) == -1)
			goto fallback;

		ext_data_control_offer_v1_receive(wl_data_control_offer, "text/plain", pipefd[1]);
		close(pipefd[1]);

		// Roundtrip ensures compositor processes the receive request
		// and writes data to the pipe before we read it.
		// Unlike flush+dispatch, roundtrip guarantees unblocking
		// (internal wl_callback forces a response from the compositor).
		wl_display_roundtrip(wl_display);

		// Read from pipe
		char buf[4096];
		int len = read(pipefd[0], buf, sizeof(buf) - 1);
		close(pipefd[0]);

		if(len > 0)
		{
			buf[len] = '\0';
			return l_strdup(buf);
		}
	}

fallback:
	// Path 2: standard wl_data_device fallback
	if(wl_data_offer && wl_data_device)
	{
		int pipefd[2];
		if(pipe(pipefd) == -1)
			return NULL;

		wl_data_offer_receive(wl_data_offer, "text/plain", pipefd[1]);
		close(pipefd[1]);

		// Roundtrip ensures compositor processes the receive
		wl_display_roundtrip(wl_display);

		char buf[4096];
		int len = read(pipefd[0], buf, sizeof(buf) - 1);
		close(pipefd[0]);

		if(len > 0)
		{
			buf[len] = '\0';
			return l_strdup(buf);
		}
	}

	return NULL;
}

// ---- w_clipboard_set_mime ----
// Save data and mime for the send callback (shared by both clipboard paths).
int w_clipboard_set_mime(const char *mime, const void *data, int size)
{
	if(!mime || !data || size <= 0)
		return -1;

	// Path 1: ext-data-control (preferred — no serial/focus requirement)
	struct ext_data_control_manager_v1 *manager = w_wayland_get_interface("ext_data_control_manager_v1");
	if(manager && wl_data_control_device)
	{
		// Release previous source if still active
		if(wl_active_source)
		{
			ext_data_control_source_v1_destroy(wl_active_source);
			wl_active_source = NULL;
		}

		struct ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source(manager);
		if(source)
		{
			wl_active_source = source;

			// Set listener to handle send requests from compositor
			ext_data_control_source_v1_add_listener(source, &data_control_source_listener, NULL);

			// Add mime types
			ext_data_control_source_v1_offer(source, mime);
			if(strcmp(mime, "text/plain") == 0)
				ext_data_control_source_v1_offer(source, "UTF8_STRING");

			// Store data for send callback
			if(wl_data_control_source_data.data)
				l_free(wl_data_control_source_data.data);
			wl_data_control_source_data.data = l_memdup(data, size);
			wl_data_control_source_data.size = size;

			// Save mime string for send callback matching
			if(wl_data_control_source_data.mime)
				l_free(wl_data_control_source_data.mime);
			wl_data_control_source_data.mime = l_strdup(mime);

			// Set as selection
			ext_data_control_device_v1_set_selection(wl_data_control_device, source);

			// Ensure compositor processes the new selection before returning
			wl_display_roundtrip(wl_display);

			wl_owns_clipboard = true;

			if(wl_debug)
				fprintf(stderr, "[wui-wayland] set clipboard mime '%s' (%d bytes) via ext-data-control\n", mime, size);

			return 0;
		}
	}

	// Path 2: standard wl_data_device fallback (requires serial from user input)
	if(wl_data_device_manager && wl_data_device && wl_last_serial > 0)
	{
		// Release previous source if still active
		if(wl_data_source_active)
		{
			wl_data_source_destroy(wl_data_source_active);
			wl_data_source_active = NULL;
		}

		struct wl_data_source *source = wl_data_device_manager_create_data_source(wl_data_device_manager);
		if(source)
		{
			wl_data_source_active = source;

			// Set listener
			wl_data_source_add_listener(source, &data_source_listener_std, NULL);

			// Add mime types
			wl_data_source_offer(source, mime);
			// GNOME clipboard manager often expects UTF8_STRING alongside text/plain
			if(strcmp(mime, "text/plain") == 0)
				wl_data_source_offer(source, "UTF8_STRING");

			// Store data for send callback (reuses same storage as ext-data-control)
			if(wl_data_control_source_data.data)
				l_free(wl_data_control_source_data.data);
			wl_data_control_source_data.data = l_memdup(data, size);
			wl_data_control_source_data.size = size;

			if(wl_data_control_source_data.mime)
				l_free(wl_data_control_source_data.mime);
			wl_data_control_source_data.mime = l_strdup(mime);

			// Set as selection — requires a serial from a user input event
			wl_data_device_set_selection(wl_data_device, source, wl_last_serial);

			wl_display_roundtrip(wl_display);

			wl_owns_clipboard = true;

			if(wl_debug)
				fprintf(stderr, "[wui-wayland] set clipboard mime '%s' (%d bytes) via std data device\n", mime, size);

			return 0;
		}
	}

	return -1;
}

int w_clipboard_set_text(const char *text)
{
	if(!text)
		return -1;

	// Reuse set_mime with text/plain
	return w_clipboard_set_mime("text/plain", text, strlen(text));
}
