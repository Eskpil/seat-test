#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <elogind/sd-bus.h>
#include <elogind/sd-login.h>

const char *seat_path;

typedef struct {
    char *seat_path;
    char *session_path;
    char *seat;
    char *id;
    sd_bus *bus;
} Seat;

void get_session_id(char **id) {
    char *xdg_session_id = getenv("XDG_SESSION_ID");
    int ret;

    if (xdg_session_id) {
        ret = sd_session_is_active(xdg_session_id);
		if (ret < 0) {
            fprintf(stderr, "[Error]: sd_session_is_active failed");
			fprintf(stderr, "[Error]: Could not check if session was active: %s", strerror(-ret));
		}
		*id = strdup(xdg_session_id); 
    } 

    ret = sd_pid_get_session(getpid(), id);

    if (ret == 0) {
        fprintf(stdout, "[Info]: There is already a session with this process.\n");
        return;
    }

    ret = sd_uid_get_display(getuid(), id);
	if (ret < 0) {
        fprintf(stderr, "[Error]: Could not get primary session for user: %s\n", strerror(-ret));
        return;
	}
}

static int find_seat_path(Seat *seat) {
    int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	ret = sd_bus_call_method(seat->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
				 "org.freedesktop.login1.Manager", "GetSeat", &error, &msg, "s",
				 seat->seat);
	if (ret < 0) {
        fprintf(stderr, "[Error]: Could not get seat: %s\n", error.message);
		goto out;
	}

	const char *path;
	ret = sd_bus_message_read(msg, "o", &path);
	if (ret < 0) {
        fprintf(stderr, "[Error]: Could not parse D-Bus response: %s\n", strerror(-ret));
		goto out;
	}
	seat->seat_path = strdup(path);

out:
    sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
    return ret;
}

static int find_session_path(Seat *seat) {
	int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	ret = sd_bus_call_method(seat->bus, "org.freedesktop.login1", "/org/freedesktop/login1",
				 "org.freedesktop.login1.Manager", "GetSession", &error, &msg, "s",
				 seat->id);
	if (ret < 0) {
        fprintf(stderr, "Could not get session: %s\n", error.message);
		goto out;
	}

	const char *path;
	ret = sd_bus_message_read(msg, "o", &path);
	if (ret < 0) {
        fprintf(stderr, "[Error]: Could not parse D-Bus response: %s\n", strerror(-ret));
		goto out;
	}
    seat->session_path = strdup(path);

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);

	return ret;
}

static int open_device(Seat *seat, const char *path, int *fd) {
	int ret;
	int tmpfd = -1;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	struct stat st;
	if (stat(path, &st) < 0) {
        fprintf(stderr, "[Error]: Could not stat path: %s: %s\n", path, strerror(errno));
		return -1;
	}

	ret = sd_bus_call_method(seat->bus, "org.freedesktop.login1", seat->session_path,
				 "org.freedesktop.login1.Session", "TakeDevice", &error, &msg, "uu",
				 major(st.st_rdev), minor(st.st_rdev));
	if (ret < 0) {
        fprintf(stderr, "[Error]: Could not take device: %s: %s\n", path, error.message);
		tmpfd = -1;
		goto out;
	}

	int paused = 0;
	ret = sd_bus_message_read(msg, "hb", &tmpfd, &paused);
	if (ret < 0) {
        fprintf(stderr, "[Error]: Could not parse D-Bus response: %s\n", strerror(-ret));
		tmpfd = -1;
		goto out;
	}

	// The original fd seems to be closed when the message is freed
	// so we just clone it.
	tmpfd = fcntl(tmpfd, F_DUPFD_CLOEXEC, 0);
	if (tmpfd < 0) {
        fprintf(stderr, "[Error]: Could not duplicate fd: %s\n", strerror(errno));
		tmpfd = -1;
		goto out;
	}

	*fd = tmpfd;

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
	return tmpfd;
}

int main(void) {
    int ret;
    Seat *seat = malloc(sizeof(Seat));

    get_session_id(&seat->id);

    printf("[Info]: Session id is: %s\n", seat->id);

    ret = sd_session_get_seat(seat->id, &seat->seat);

    if (ret < 0) {
        fprintf(stderr, "[Error]: Could not get the session seat: %s\n", strerror(-ret));
        return 1;
    }

    printf("[Info]: Seat is: %s\n", seat->seat);

    ret = sd_bus_default_system(&seat->bus);

    if (ret < 0) {
        fprintf(stderr, "[Error]: Could not get the default system: %s\n", strerror(-ret));
        return 1;
    }

    if (find_session_path(seat) < 0) {
        return 1;
    }

    printf("[Info]: Session path is: %s\n", seat->session_path);

    if (find_seat_path(seat) < 0) {
        return 1;
    }

    printf("[Info]: Seat path is: %s\n", seat->seat_path);

    int fd;
    // const char *path = "/dev/input/by-path/pci-0000:01:00.0-usb-0:7:1.0-event-kbd";
    const char *path = "/dev/input/event0";

    if (open_device(seat, path, &fd) < 0) {
        return 1;
    }

    printf("[Info]: Path: %s: %d\n", path, fd);

    free(seat);

    return 0;
}
