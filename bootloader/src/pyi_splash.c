/*
 * ****************************************************************************
 * Copyright (c) 2013-2023, PyInstaller Development Team.
 *
 * Distributed under the terms of the GNU General Public License (version 2
 * or later) with exception for distributing the bootloader.
 *
 * The full license is in the file COPYING.txt, distributed with this software.
 *
 * SPDX-License-Identifier: (GPL-2.0-or-later WITH Bootloader-exception)
 * ****************************************************************************
 */

#ifdef _WIN32
    #include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PyInstaller headers */
#include "pyi_global.h"
#include "pyi_archive.h"
#include "pyi_utils.h"
#include "pyi_path.h"
#include "pyi_splash.h"

/**
 * Splash Screen Feature
 *
 * A splash screen is a graphical window in which a program-defined screen
 * is displayed. It is normally used to give the user visual feedback,
 * indicating that the program has been started.
 *
 * In this file the splash screen feature as of discussed in pyinstaller#4354 is
 * implemented. To show a splash screen the Tk library is used. Tk is accessed by
 * and distributed with Tcl inside the python standard library (as of python 3.8
 * with Tcl/Tk 8.6). Python uses Tcl/Tk in the tkinter module. Tkinter is a wrapper
 * between python and Tcl, so using tkinter will use Tcl/Tk. Because tkinter is
 * distributed with any common python installation and it is cross-platform,
 * it is also used for this splash screen.
 *
 * If splash screen is enabled, PyInstaller bundles all necessary Tcl/Tk resources
 * for inclusion in the frozen application. This results in a slightly bigger
 * application distribution if a splash screen is used, but it is assumed to be negligible.
 *
 * Tcl is a simple high-level programming language like python. It is often embedded into
 * C application for prototyping. Together with Tk (called Tcl/Tk) it is a very powerful
 * tool to build graphical user interfaces and is often used to give C applications
 * a GUI, since it is easy to embed.
 *
 * The implementation of splash screen looks for splash screen resources in the frozen
 * application's archive; if found, it dynamically loads the Tcl and Tk shared libraries,
 * initializes a minimal Tcl/Tk environment, and runs the splash screen in it.
 *
 * Only threaded Tcl is supported; i..e, Tcl had to be compiled with the --enable-threads
 * flag, which is it by default on Windows and MacOS. Many Linux distributions also come
 * with threaded Tcl installation, although it is not guaranteed. PyInstaller checks at
 * build time if Tcl is threaded and raises an error if it is not.
 */


/* Mutexes used for thread safe access to variables */
static Tcl_Mutex status_mutex;
static Tcl_Mutex call_mutex;

/* This mutex/condition is to hold the bootloader until the splash screen
 * has been started */
static Tcl_Mutex start_mutex;
static Tcl_Condition start_cond;

/* These are used to close the splash screen from the main thread. */
static Tcl_Condition exit_wait;
static Tcl_Mutex exit_mutex;
static bool exitMainLoop;

/* Forward declarations */
static Tcl_ThreadCreateProc _splash_init;
typedef struct Splash_Event Splash_Event;


/*
 * Search the PKG/CArchive for splash screen resources and return a
 * pointer to buffer that contains its data. If no splash screen
 * resources are found, NULL is returned.
 *
 * The splash screen resources entry is identified in the PKG/CArchive
 * by the type code 'ARCHIVE_ITEM_SPLASH'.
 *
 * The SPLASH_DATA_HEADER structure is, if loaded from archive,
 * in network/big endian, and must be converted to system endianness.
 */
static SPLASH_DATA_HEADER *
_pyi_splash_find_data_header(ARCHIVE_STATUS *archive)
{
    SPLASH_DATA_HEADER *header = NULL;
    const TOC *toc_entry;

    toc_entry = archive->tocbuff;
    while (toc_entry < archive->tocend) {
        if (toc_entry->typcd == ARCHIVE_ITEM_SPLASH) {
            header = (SPLASH_DATA_HEADER *)pyi_arch_extract(archive, toc_entry);
            break;
        }
        toc_entry = pyi_arch_increment_toc_ptr(archive, toc_entry);
    }

    return header;
}

/*
 * Initialize the splash screen context by reading its data and defining
 * the necessary paths and resources.
 */
int
pyi_splash_setup(SPLASH_CONTEXT *splash, const PYI_CONTEXT *pyi_ctx)
{
    SPLASH_DATA_HEADER *data_header;

    /* Read splash resources entry from the archive */
    data_header = _pyi_splash_find_data_header(pyi_ctx->archive);
    if (data_header == NULL) {
        return -1; /* No splash resources */
    }
    VS("SPLASH: found splash screen resources.\n");

    /* In onedir mode, Tcl/Tk dependencies (shared libraries, .tcl files)
     * are located directly in top-level application directory. In onefile
     * mode, they are extracted into sub-directory under (temporary/ephemeral)
     * top-level application directory. The sub-directory name is controlled
     * by the `rundir` value in SPLASH_DATA_HEADER.
     *
     * NOTE: the name fields in SPLASH_DATA_HEADER are 16 characters wide,
     * and are *implicitly* NULL terminated; the build process uses zero
     * padding and is ensuring that strings themselves have no more than
     * 15 characters long. */

    /* Full path to run-time directory that contains Tcl/Tk dependencies. */
    if (pyi_ctx->is_onefile) {
        /* Onefile mode: sub-directory under (temporary/ephemeral) top-level
         * application directory. */
        if (pyi_path_join(splash->splash_dependencies_dir, pyi_ctx->application_home_dir, data_header->rundir) == NULL) {
            OTHERERROR("SPLASH: length of run-time splash directory path exceeds maximum path length!\n");
            free(data_header);
            return -1;
        }
    } else {
        /* Onedir mode: top-level application directory */
        /* NOTE: the path length is guaranteed to fit PATH_MAX */
        snprintf(splash->splash_dependencies_dir, PATH_MAX, "%s", pyi_ctx->application_home_dir);
    }

    /* Tcl shared library */
    if (pyi_path_join(splash->tcl_libpath, splash->splash_dependencies_dir, data_header->tcl_libname) == NULL) {
        OTHERERROR("SPLASH: length of Tcl shared library path exceeds maximum path length!\n");
        free(data_header);
        return -1;
    }

    /* Tk shared library */
    if (pyi_path_join(splash->tk_libpath, splash->splash_dependencies_dir, data_header->tk_libname) == NULL) {
        OTHERERROR("SPLASH: length of Tk shared library path exceeds maximum path length!\n");
        free(data_header);
        return -1;
    }

    /* Tk modules directory */
    if (pyi_path_join(splash->tk_lib, splash->splash_dependencies_dir, data_header->tk_lib) == NULL) {
        OTHERERROR("SPLASH: length of Tk shared library path exceeds maximum path length!\n");
        free(data_header);
        return -1;
    }

    /* Copy the script into a buffer owned by SPLASH_STATUS */
    splash->script_len = pyi_be32toh(data_header->script_len);
    splash->script = (char *)calloc(1, splash->script_len + 1);

    /* Copy the image into a buffer owned by SPLASH_STATUS */
    splash->image_len = pyi_be32toh(data_header->image_len);
    splash->image = (char *)malloc(splash->image_len);

    /* Copy the requirements array into a buffer owned by SPLASH_STATUS */
    splash->requirements_len = pyi_be32toh(data_header->requirements_len);
    splash->requirements = (char *)malloc(splash->requirements_len);

    if (splash->script == NULL || splash->image == NULL || splash->requirements == NULL) {
        FATALERROR("Could not allocate memory for splash screen resources.\n");
        free(data_header);
        return -1;
    }

    /* Copy the data into their respective fields */
    memcpy(
        splash->script,
        ((char *)data_header) + pyi_be32toh(data_header->script_offset),
        splash->script_len
    );
    memcpy(
        splash->image,
        ((char *)data_header) + pyi_be32toh(data_header->image_offset),
        splash->image_len
    );
    memcpy(
        splash->requirements,
        ((char *)data_header) + pyi_be32toh(data_header->requirements_offset),
        splash->requirements_len
    );

    /* Free raw header data */
    free(data_header);

    return 0;
}

/*
 * Start the splash screen.
 *
 * As this uses bound functions from Tcl/Tk shared libraries, it must
 * be called after the shared libaries have been loaded and their
 * symbols bound.
 *
 * The splash screen needs to run in a separate thread, otherwise
 * the event loop of the GUI would block the extraction. We only
 * implement this for threaded tcl, since many threading functions
 * from tcl are only available, if tcl was compiled with threading
 * support.
 *
 * In order to start the splash screen a new thread is created, in which
 * the internal function _splash_init is called. This function will setup
 * the environment for the splash screen.
 *
 * If the thread was created successfully, the return value will be 0,
 * otherwise a non zero number is returned. Note that a return code of
 * 0 does not necessarily mean, that Tcl/Tk was successfully initialized.
 */
int
pyi_splash_start(SPLASH_CONTEXT *splash, const char *executable)
{
    PI_Tcl_MutexLock(&status_mutex);

    /* Make sure shared libraries have been loaded and their symbols
     * bound. */
    if (!splash->dlls_fully_loaded) {
        return -1;
    }

    /* This functions needs to be called before everything else is done
     * with Tcl, otherwise the behavior of Tcl is undefined. */
    PI_Tcl_FindExecutable(executable);

    /* We try to create a new thread (in which the Tcl interpreter will run) with
     * methods provided by Tcl. This function will return TCL_ERROR if it is
     * either not implemented (Tcl is not threaded) or an error occurs.
     * Since we only support threaded Tcl, the error is fatal. */
    if (PI_Tcl_CreateThread(
        &splash->thread_id, /* location to store thread ID */
        _splash_init, /* procedure/function to run in the new thread */
        splash, /* parameters to pass to procedure */
        0, /* use default stack size */
        0 /* no flags */
    ) != TCL_OK) {
        FATALERROR("SPLASH: Tcl is not threaded. Only threaded Tcl is supported.\n");
        PI_Tcl_MutexUnlock(&status_mutex);
        pyi_splash_finalize(splash);
        return -1;
    }
    PI_Tcl_MutexLock(&start_mutex);
    PI_Tcl_MutexUnlock(&status_mutex);

    VS("SPLASH: created thread for Tcl interpreter.\n");

    /* To avoid a race condition between the tcl and python interpreter
     * we need to wait until the splash screen has been started. We lock
     * here until the tcl thread notified us, that it has finished starting up.
     * See discarded idea in pyi_splash python module */
    PI_Tcl_ConditionWait(&start_cond, &start_mutex, NULL);
    PI_Tcl_MutexUnlock(&start_mutex);
    PI_Tcl_ConditionFinalize(&start_cond);
    VS("SPLASH: splash screen started.\n");

    return 0;
}

/*
 * Extract the necessary parts of the splash screen resources from
 * the PKG/CArchive, if they are bundled (i.e., onefile mode). No-op
 * in onedir mode.
 *
 * Since these extracted files would collide with the files that are
 * extracted in pyi_launch_extract_files_from_archive, we put the splash
 * screen files into a subdirectory inside the application's (temporary)
 * top-level directory, which we refer to as "splash dependencies
 * directory". The name of this subdirectory is controlled by the
 * SPLASH_DATA_HEADER "rundir" field, which is ensured to not collide
 * with any custom directory that is part of frozen application.
 *
 * Unpacking into a subdirectory creates a small inefficiency, because
 * the loop in pyi_launch_extract_files_from_archive unpacks these files
 * again later.
 */
int
pyi_splash_extract(SPLASH_CONTEXT *splash, const PYI_CONTEXT *pyi_ctx)
{
    const ARCHIVE_STATUS *archive = pyi_ctx->archive;
    const TOC *toc_entry;
    const char *filename = NULL;
    size_t pos;

    /* No-op in onedir mode */
    if (!pyi_ctx->is_onefile) {
        return 0;
    }

    /* Make sure splash dependencies directory exists */
    if (pyi_path_exists(splash->splash_dependencies_dir) == 0) {
        if (pyi_path_mkdir(splash->splash_dependencies_dir) < 0) {
            FATALERROR(
                "SPLASH: could not create splash dependencies directory %s.\n",
                splash->splash_dependencies_dir
            );
            return -1;
        }
    }

    /* Iterate over the requirements array */
    for (pos = 0; pos < (size_t)splash->requirements_len; pos += strlen(filename) + 1) {
        /* Read filename from requirements array */
        filename = splash->requirements + pos;

        /* Look-up entry in archive's TOC */
        toc_entry = pyi_arch_find_by_name(archive, filename);
        if (toc_entry == NULL) {
            FATALERROR("SPLASH: could not find requirement %s in archive.\n", filename);
            return -1;
        }

        /* Extract file into the splash dependencies directory */
        if (pyi_arch_extract2fs(archive, toc_entry, splash->splash_dependencies_dir)) {
            FATALERROR("SPLASH: could not extract requirement %s.\n", toc_entry->name);
            return -2;
        }
    }

    return 0;
}

/* Load Tcl/Tk shared libraries and bind required symbols (functions). */
int
pyi_splash_load_shared_libaries(SPLASH_CONTEXT *splash)
{
    splash->dlls_fully_loaded = false;

    VS("SPLASH: loading Tcl library from: %s\n", splash->tcl_libpath);
    VS("SPLASH: loading Tk library from: %s\n", splash->tk_libpath);

    splash->dll_tcl = pyi_utils_dlopen(splash->tcl_libpath);
    splash->dll_tk = pyi_utils_dlopen(splash->tk_libpath);

    if (splash->dll_tcl == 0 || splash->dll_tk == 0) {
        FATALERROR("SPLASH: failed to load Tcl/Tk shared libraries!\n");
        return -1;
    }

    /* Bind symbols */
    if (pyi_splashlib_bind_functions(splash->dll_tcl, splash->dll_tk) < 0) {
        return -1;
    }

    /* Tcl/Tk shared libraries are fully loaded and their symbols bound,
     * so it is safe to use them. */
    splash->dlls_fully_loaded = true;

    return 0;
}

/*
 * Finalizes the splash screen.
 * This function is normally called at exiting the splash screen.
 */
int
pyi_splash_finalize(SPLASH_CONTEXT *splash)
{
    if (splash == NULL) {
        return 0;
    }

    /* If we failed to fully attach Tcl/Tk libraries (either because one
     * of the libraries failed to load, or because we failed to load one
     * of the symbols from the libraries), we are guaranteed to be in the
     * bootloader thread, and we only need to clean up the shared libraries,
     * in case any of them were successfully loaded. */
    if (splash->dlls_fully_loaded != true) {
        if (splash->dll_tcl != NULL) {
            pyi_utils_dlclose(splash->dll_tcl);
            splash->dll_tcl = NULL;
        }

        if (splash->dll_tk != NULL) {
            pyi_utils_dlclose(splash->dll_tk);
            splash->dll_tk = NULL;
        }

        return 0;
    }

    if (splash->thread_id == PI_Tcl_GetCurrentThread()) {
        /* We are in the Tcl interpreter's thread. */
        if (splash->interp != NULL) {
            /* We can only call this function safely from the Tcl
             * interpreter's thread. */
            PI_Tcl_DeleteInterp(splash->interp);
            /* Prevent dangling pointers. */
            splash->interp = NULL;
        }
    } else {
        /* We are in the bootloader's main thread. */
        if (splash->interp != NULL) {
            /* If the Tcl thread still exists, we notify it and wait
             * for it to exit. */
            PI_Tcl_MutexLock(&exit_mutex);
            exitMainLoop = true;
            /* We need to post a fake event into the event queue in order
             * to unblock Tcl_DoOneEvent, so the Tcl main loop can exit. */
            pyi_splash_send(splash, true, NULL, NULL);
            PI_Tcl_ConditionWait(&exit_wait, &exit_mutex, NULL);
            PI_Tcl_MutexUnlock(&exit_mutex);
            PI_Tcl_ConditionFinalize(&exit_wait);
        }
        /* This function should only be called after python has been
         * destroyed with Py_Finalize. Tcl/Tk/tkinter do **not** support
         * multiple instances of themselves due to restrictions of Tcl
         * (for reference see _tkinter PyMethodDef m_size field or
         * disabled registration of Tcl_Finalize inside _tkinter.c)
         * The python program may have imported tkinter, which keeps
         * its own tcl interpreter. If we finalized Tcl here, the
         * Tcl interpreter of tkinter would also be finalized, resulting
         * in a weird state of tkinter. */
        PI_Tcl_Finalize();

        /* If the shared libraries are not yet unloaded, unload them here,
         * as otherwise their files cannot be deleted. */
        if (splash->dll_tcl != NULL) {
            pyi_utils_dlclose(splash->dll_tcl);
            splash->dll_tcl = NULL;
        }

        if (splash->dll_tk != NULL) {
            pyi_utils_dlclose(splash->dll_tk);
            splash->dll_tk = NULL;
        }
    }

    return 0;
}

/*
 * Allocate memory for splash status
 */
SPLASH_CONTEXT *
pyi_splash_context_new()
{
    SPLASH_CONTEXT *splash;

    splash = (SPLASH_CONTEXT *)calloc(1, sizeof(SPLASH_CONTEXT));

    if (splash == NULL) {
        FATAL_PERROR("calloc", "Could not allocate memory for SPLASH_CONTEXT.\n");
    }

    return splash;
}

/*
 * Free memory allocated for the splash context structure (the memory
 * allocated for its heap-allocated fields, as well as the structure
 * itself). The splash context structure is passed via pointer to
 * location that stores the structure - this location is also cleared
 * to NULL.
 */
void
pyi_splash_context_free(SPLASH_CONTEXT **splash_ref)
{
    SPLASH_CONTEXT *splash = *splash_ref;

    *splash_ref = NULL;

    if (splash == NULL) {
        return;
    }

    free(splash->script);
    free(splash->image);
    free(splash->requirements);

    free(splash);
}

/* ----------------------------------------------------------------------------------------- */

/* We can pass data to Tcl interpreter thread or execute functions in it
 * by implementing custom Tcl events. */
struct Splash_Event
{
    Tcl_Event ev; /* must be first */
    SPLASH_CONTEXT *splash;
    /* We may wait for the interpreter thread to complete to get
     * a result. For this we use the done condition. The behavior
     * of result and the condition are only defined, if async is false. */
    bool async;
    Tcl_Condition *done;
    int *result;
    /* We let the caller decide which function to execute in the interpreter
     * thread, so we pass an function to the interpreter to execute.
     * The function receives the current SPLASH_CONTEXT and user_data */
    pyi_splash_event_proc *proc;
    const void *user_data;
};

/*
 * We encapsulate the way we post the events to the interpreter
 * thread.
 *
 * In order to safely receive the result, we created a mutex called
 * call_mutex, which controls access to the result field of the Splash_Event
 * (technically, it controls the access to whole Splash_Event, but we only
 * care about the result field). If async is false, we block until the
 * interpreter thread serviced the event.
 */
static void
_splash_event_send(
    SPLASH_CONTEXT *splash,
    Tcl_Event *ev,
    Tcl_Condition *cond,
    Tcl_Mutex *mutex,
    bool async
)
{
    PI_Tcl_MutexLock(mutex);
    PI_Tcl_ThreadQueueEvent(splash->thread_id, ev, TCL_QUEUE_TAIL);
    PI_Tcl_ThreadAlert(splash->thread_id);

    if (!async) {
        PI_Tcl_ConditionWait(cond, mutex, NULL); /* Wait for the result */
    }

    PI_Tcl_MutexUnlock(mutex);
}

/*
 * This is a wrapper function for the custom proc passed via
 * Splash_Event. It encapsulates the logic to safely return
 * the result of the custom procedure passed to pyi_splash_send.
 * If pyi_splash_send was called with async = true, the result
 * of the custom procedure is discarded; if false was supplied,
 * the variable pointer by result will be updated.
 *
 * Note: this function is executed inside the Tcl interpreter thread.
 */
static int
_splash_event_proc(Tcl_Event *ev, int flags)
{
    int rc = 0;
    Splash_Event *splash_event;

    splash_event = (Splash_Event *)ev;

    /* Call the custom procedure passed to pyi_splash_send */
    if (splash_event->proc != NULL) {
        rc = (splash_event->proc)(splash_event->splash, splash_event->user_data);
    }

    if (!splash_event->async) {
        /* In synchronous mode, the caller thread is waiting on the
         * wait condition. Notify it that the function call has finished. */
        PI_Tcl_MutexLock(&call_mutex);

        *splash_event->result = rc;

        PI_Tcl_ConditionNotify(splash_event->done);
        PI_Tcl_MutexUnlock(&call_mutex);
    }

    /* Not an error code; value 1 indicates that event has been processed. */
    return 1;
}

/*
 * To update the splash screen text with the name of the currently-processed
 * TOC entry, we schedule a Splash_Event into the Tcl interpreters event queue.
 *
 * This function will update the variable "status_text", which updates the label
 * on the splash screen. We schedule this function in async mode, meaning
 * the main (bootloader) thread does not wait for this function to finish
 * its execution.
 *
 * Note: this function is executed inside the Tcl interpreter thread.
 */
static int
_pyi_splash_progress_update(SPLASH_CONTEXT *splash, const void *user_data)
{
    const TOC *toc_entry = (const TOC *)user_data;
    PI_Tcl_SetVar2(splash->interp, "status_text", NULL, toc_entry->name, TCL_GLOBAL_ONLY);
    return 0;
}

/*
 * To update the text on the splash screen (optionally) we provide
 * this function, which enqueues an event for the Tcl interpreter
 * thread to service. We update the text based on the name gave by TOC
 * entry.
 *
 * This function is called from bootloader's main thread, namely from
 * the pyi_launch_extract_files_from_archive while it extracts files
 * from the executable-embedded archive.
 */
int
pyi_splash_update_prg(SPLASH_CONTEXT *splash, const TOC *toc_entry)
{
    /* We enqueue the _pyi_splash_progress_update function into the tcl
     * interpreter event queue in async mode, ignoring the return value. */
    return pyi_splash_send(splash, true, toc_entry, _pyi_splash_progress_update);
}

/*
 * To enqueue a function (proc) to be serviced by the Tcl interpreter
 * (therefore interacting with the interpreter), we provide this function
 * to execute the procedure in the Tcl thread.
 *
 * This function supports two execution modes:
 *  - async: activated by setting async to true. In this case the function
 *           is enqueued for processing, but we do not wait for it to be
 *           processed, therefore not blocking the caller (returning after
 *           the function has been scheduled).
 *  - sync:  in this mode the function blocks the calling thread until
 *           the function has been serviced by the Tcl interpreter.
 *           The return value of the enqueued function will be the return
 *           value of this function.
 *
 * All function executed inside the Tcl interpreter thread are holding
 * the status mutex, meaning they can safely modify the SPLASH_CONTEXT.
 */
int
pyi_splash_send(SPLASH_CONTEXT *splash, bool async, const void *user_data, pyi_splash_event_proc proc)
{
    int rc = 0;
    Splash_Event *ev;
    Tcl_Condition cond = NULL;

    /* Tcl will free this event once it was serviced. */
    ev = (Splash_Event *)PI_Tcl_Alloc(sizeof(Splash_Event));

    ev->ev.proc = (Tcl_EventProc *)_splash_event_proc;
    ev->splash = splash;

    /* Needed for synchronous return values. */
    ev->async = async;
    ev->done = &cond;
    ev->result = &rc;

    /* The custom procedure to be called. */
    ev->proc = proc;
    ev->user_data = user_data;

    _splash_event_send(splash, (Tcl_Event *)ev, &cond, &call_mutex, async);

    if (!async) {
        PI_Tcl_ConditionFinalize(&cond);
    }
    return rc;
}

/* ----------------------------------------------------------------------------------------- */

/*
 * This is the command handler for the Tcl command `tclInit`
 * By default, `Tcl_Init` defines a internal `tclInit` procedure, which
 * is called in order to find the Tcl standard library. If a `tclInit`
 * command is created/registered by the wrapping C code, it will be called
 * instead.
 *
 * We override the internal function, because we want to run Tcl in a very
 * minimal environment and do not want to initialize the standard library.
 */
static int
_tclInit_Command(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    /**
     * This function would normally do a search in some common and
     * specific paths to find an `init.tcl` file. Once found, every script
     * next to it would be executed (`auto.tcl`, `clock.tcl`, etc.) to
     * define the standard library.
     * This initialization script would normally set `$auto_path` to be
     * the folder where `init.tcl` was found, usually the `tclX.Y` directory
     * inside python's Tcl distribution directory.
     */
    return TCL_OK;
}

static int
_tcl_findLibrary_Command(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    /**
     * This function is normally defined inside `auto.tcl`, and searches
     * for modules that Tcl provides via its standard library. It
     * performs a canonical search through different places, for example
     * relative to `$auto_path` and `$tcl_library`.
     *
     * We replace this function with custom implementation in order to run
     * a minimal Tcl environment. This implementation resolves only `tk.tcl`,
     * which is required for Tk initialization in `Tk_Init`:
     *
     * https://github.com/tcltk/tk/blob/core_8_6_7/generic/tkWindow.c#L3326
     *
     * Original function description in auto.tcl:
     *
     *  tcl_findLibrary --
     * 	This is a utility for extensions that searches for a library directory
     * 	using a canonical searching algorithm. A side effect is to source the
     * 	initialization script and set a global library variable.
     *  Arguments:
     *  	basename	Prefix of the directory name, (e.g., "tk")
     * 	    version		Version number of the package, (e.g., "8.0")
     * 	    patch		Patchlevel of the package, (e.g., "8.0.3")
     * 	    initScript	Initialization script to source (e.g., tk.tcl)
     * 	    enVarName	environment variable to honor (e.g., TK_LIBRARY)
     * 	    varName		Global variable to set when done (e.g., tk_library)
     */
    int rc;
    SPLASH_CONTEXT *splash;
    char initScriptPath[PATH_MAX];

    splash = (SPLASH_CONTEXT *)clientData;

    /* In our minimal environment, this function is only called once,
     * from Tk_Init. So we only implement the behavior for Tk. Other
     * libraries are therefore not supported. We do not check the version
     * of `tk`, since the library packed by PyInstaller at build time is
     * guaranteed to be compatible. */
    if (strncmp(PI_Tcl_GetString(objv[4]), "tk.tcl", 64) == 0) {
        pyi_path_join(initScriptPath, splash->tk_lib, PI_Tcl_GetString(objv[4]));
        PI_Tcl_SetVar2(interp, "tk_library", NULL, splash->tk_lib, TCL_GLOBAL_ONLY);
        rc = PI_Tcl_EvalFile(interp, initScriptPath);
        return rc;
    }

    /* We do not expect this function to be called for any other library,
     * but just in case, return that the library was not found. */
    return TCL_ERROR;
}

/*
 * The `source` command takes the contents of a specified file or resource
 * and passes it to the Tcl interpreter as a text script.
 *
 * We override this command, because we run Tcl in a minimal environment, in
 * which some files may not be included. At build time, PyInstaller includes
 * only files that are necessary to run the splash screen. If the default
 * `source` command encountered a non-existent file, it would throw an
 * error, which we do not want. In our custom implementation, we therefore
 * silently ignore missing files.
 */
static int
_tcl_source_Command(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    /* In `_splash_init`, we renamed the original `source` command to `_source`
     * in order  to keep its functionality available. As we know that we are
     * running an error-free script,  we do not do the checks for a valid
     * command, or at least we do it with the original `source` command. */
    Tcl_Obj **_source_objv;
    int rc;
    int i;

    /* Check if the file to be sourced exists. The filename
     * is always the last (objc-1) parameter passed to the command */
    if (pyi_path_exists(PI_Tcl_GetString(objv[objc - 1]))) {
        /* Create a new objv array for the original source command
         * named _source. */
        _source_objv = (Tcl_Obj **)PI_Tcl_Alloc(sizeof(Tcl_Obj *) * objc);
        _source_objv[0] = PI_Tcl_NewStringObj("_source", -1);

        for (i = 1; i < objc; i++) {
            _source_objv[i] = objv[i];
        }

        /* Execute `_source` with the given arguments */
        rc = PI_Tcl_EvalObjv(interp, objc, _source_objv, 0);
        PI_Tcl_Free((char *) _source_objv);

        return rc;
    }

    /* If the file does not exist, we return OK */
    return TCL_OK;

}

/*
 * The default Tcl `exit` command terminates the whole application;
 * we override it to just exit the main loop, so that the main thread
 * with python interpreter can continue running.
 */
static int
_tcl_exit_Command(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[])
{
    exitMainLoop = true;
    return TCL_OK;
}

/*
 * This function is executed inside a new thread, in which the Tcl
 * interpreter will run.
 *
 * We create and initialize the Tcl interpreter in this thread since
 * threaded Tcl locks a interpreter to a specific thread at creation.
 * In order to be thread-safe during initialization, we use a Tcl_Mutex
 * called `status_mutex` to lock access to the SPLASH_CONTEXT. This mutex
 * is initially acquired at the point where this thread is created (i.e.,
 * in the main thread, in `pyi_splash_start`). After the main thread
 * finished creating this thread, the `status_mutex` is released, and
 * this thread gets to hold it. It will only be unlocked after the splash
 * screen is closed. This means that all function called through
 * `pyi_splash_send` are called with mutex held, and therefore they are
 * safe to modify SPLASH_CONTEXT.
 *
 * Note: This function will run/setup the Tcl interpreter thread.
 */
static Tcl_ThreadCreateType
_splash_init(ClientData client_data)
{
    int err = 0;
    SPLASH_CONTEXT *splash;
    Tcl_Obj *image_data_obj;

    PI_Tcl_MutexLock(&status_mutex);

    splash = (SPLASH_CONTEXT *)client_data;
    exitMainLoop = false;

    splash->interp = PI_Tcl_CreateInterp();

    if (splash->thread_id == NULL) {
        /* This should never happen, but as a backup we set the field in here. */
        splash->thread_id = PI_Tcl_GetCurrentThread();
    }

    /* In order to run a minimal Tcl interpreter, we override the `tclInit`
     * command, which is called by Tcl_Init().
     * This is a supported way of modifying Tcl's startup behavior. */
    err |= PI_Tcl_CreateObjCommand(
        splash->interp,
        "tclInit",
        _tclInit_Command,
        splash,
        NULL
    ) == NULL;

    /* Tk_Init calls the Tcl standard library function 'tcl_findLibrary' */
    err |= PI_Tcl_CreateObjCommand(
        splash->interp,
        "tcl_findLibrary",
        _tcl_findLibrary_Command,
        splash,
        NULL
    ) == NULL;

    /* We override the exit command to terminate only this thread and not
     * the whole application. */
    err |= PI_Tcl_CreateObjCommand(
        splash->interp,
        "exit",
        _tcl_exit_Command,
        splash,
        NULL
    ) == NULL;

    /* Replace `source` command for use in minimal environment. */
    PI_Tcl_EvalEx(splash->interp, "rename ::source ::_source", -1, 0);
    err |= PI_Tcl_CreateObjCommand(
        splash->interp,
        "source",
        _tcl_source_Command,
        splash,
        NULL
    ) == NULL;

    /* We OR return values of the Tcl_CreateObjCommand function because
     * if one of them fails, the splash screen should be aborted (and
     * generally, if one fails, all of them should fail). */
    if (err) {
        VS("TCL: failed to create setup commands. Error: %s\n", PI_Tcl_GetString(PI_Tcl_GetObjResult(splash->interp)));
        goto cleanup;
    }

    /* Initialize Tcl/Tk */
    err |= PI_Tcl_Init(splash->interp);

    if (err) {
        VS("SPLASH: error while initializing Tcl: %s\n", PI_Tcl_GetString(PI_Tcl_GetObjResult(splash->interp)));
    }

    err |= PI_Tk_Init(splash->interp);

    if (err) {
        VS("SPLASH: error while initializing Tk: %s\n", PI_Tcl_GetString(PI_Tcl_GetObjResult(splash->interp)));
    }

    if (err) {
        goto cleanup;
    }

    /* Display version of Tcl and Tk for debugging purposes. */
    VS(
        "SPLASH: running Tcl version %s and Tk version %s.\n",
        PI_Tcl_GetVar2(splash->interp, "tcl_patchLevel", NULL, TCL_GLOBAL_ONLY),
        PI_Tcl_GetVar2(splash->interp, "tk_patchLevel", NULL, TCL_GLOBAL_ONLY)
    );

    /* Extract the image from the splash resources, and pass it to Tcl/Tk
     * via the `_image_data` variable. */
    image_data_obj = PI_Tcl_NewByteArrayObj(splash->image, splash->image_len);
    PI_Tcl_SetVar2Ex(splash->interp, "_image_data", NULL, image_data_obj, TCL_GLOBAL_ONLY);

    /* Tcl/Tk creates a copy of the image, so we can free our buffer */
    free(splash->image);
    splash->image = NULL;

    err = PI_Tcl_EvalEx(splash->interp, splash->script, splash->script_len, TCL_GLOBAL_ONLY);

    if (err) {
        VS("SPLASH: Tcl error: %s\n", PI_Tcl_GetString(PI_Tcl_GetObjResult(splash->interp)));
    }

    /* We need to notify the bootloader main thread that the splash screen
     * has been started and fully setup */
    PI_Tcl_MutexLock(&start_mutex);
    PI_Tcl_ConditionNotify(&start_cond);
    PI_Tcl_MutexUnlock(&start_mutex);

    /* Main loop.
     * we exit this loop from within tcl. */
    while (PI_Tk_GetNumMainWindows() > 0 && !exitMainLoop) {
        /* Tcl_DoOneEvent blocks this loop until an event is posted into this threads
         * event queue, only after that the condition exitMainLoop is checked again.
         * To unblock this loop while the splash screen is not visible (e.g. receives
         * no events) we post a fake event at finalization (in pyi_splash_finalize) */
        PI_Tcl_DoOneEvent(0);
    }

cleanup:
    pyi_splash_finalize(splash);
    PI_Tcl_MutexUnlock(&status_mutex);

    /* In case the startup fails the main thread should continue; in
     * normal startup this segment will notify no waiting condition. */
    PI_Tcl_MutexLock(&start_mutex);
    PI_Tcl_ConditionNotify(&start_cond);
    PI_Tcl_MutexUnlock(&start_mutex);

    /* Must be done before exit_wait condition is notified, because
     * we need to ensure that the main thread (which is waiting on it)
     * does not unload the Tcl library before we're done with this
     * Tcl_FinalizeThread() call. */
    PI_Tcl_FinalizeThread();

    /* We notify all conditions waiting for this thread to exit, if
     * there are any. */
    PI_Tcl_MutexLock(&exit_mutex);
    PI_Tcl_ConditionNotify(&exit_wait);
    PI_Tcl_MutexUnlock(&exit_mutex);

    TCL_THREAD_CREATE_RETURN;
}
