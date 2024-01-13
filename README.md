# PicoMsg
**Miniature Message Passing IPC System (Single Header C++ File)**

### Licence:
https://creativecommons.org/licenses/by/4.0/ I chose this licence, for it's strict requirement of attribution, while still being quite permissive.

### About
PicoMsg is a single-header, thread-safe, simple and fast message-passing library.

PicoMsg uses the single-producer, single-consumer approach. PicoMsg is simpler and smaller than nanomsg and zeromq, at around 450 SLOC.

PicoMsg uses two threads behind the scenes, to read and write. PicoMsg will communicate with sockets across processes. But if you are using PicoMsg to communicate within a process, it uses direct memory sharing! Much faster!

### Usage

PicoMsg is almost always non-blocking. The default buffer sizes are: Send=1MB, Receive=1MB, Queue <= 8MB (it grows). If your program is busy sending a lot of data, it probably won't block.

The other side, will have two worker-threads that slurp up all your data. One thing to remember, is that you can't send messages bigger than your buffers. That limits us to 1MB-4 bytes per message, by default. PicoMsg will send and get multiple messages per read/send event, if multiple are available.

PicoMsg is open source. If the default behaviour is not good enough for you, feel free to tweak it! It shouldn't be hard to make the buffer-size growable if you really need that... or default to lower-sizes if you prefer.

### Building

If you are using `Speedie` (my language) it will expect PicoMsg to be at `/usr/local/include/PicoMsg/`, and it **might** be good (depending on your needs) to have PicoMsg there anyhow as you can include it from all your projects. But PicoMsg will work fine no matter where it is placed.

	cd /usr/local/include/PicoMsg/
	g++ PicoTest.cpp -o picotest -std=c++20 -Os

Then you can run the executable using "`picotest 1`" or "`picotest 2`" or "`picotest 3`".

# API

### Initialisation / Destruction

Start by calling `PicoCreate`, then call either `PicoStartChild `, `PicoStartThread ` or `PicoStartFork `. Call `PicoDestroy` when you are finsished.

**`PicoComms* PicoCreate ()`**   :   Creates your message-passer.

**`bool PicoStartThread (PicoComms* M, PicoThreadFn fn)`**   :   Creates a new thread, using the function "fn", and passes a new PicoComms object to it!. Returns false if any error occurred.

**`pid_t PicoStartFork (PicoComms* M)`**   :   This will fork your app, and then connect the two apps with PicoMsg. Returns the result from fork(). Same numbers as `fork()` returns. Such as that -1 means an error occurred.

**`void PicoDestroy (PicoComms* M)`**   :   Destroys the PicoComms object, and reclaims memory. Also closes the other side.

### Communication

**`bool PicoSend (PicoComms* M, PicoMessage Msg, bool CanWait=false)`**   :   Sends the message. The data is copied to internal buffers so you do not need to hold onto it after send. If `CanWait` is false and there is no buffer space, this function returns `false`. If `CanWait` is true, it will block until the timeout is reached. See the ["configuration"](#Configuration) section about how to change the timeout.

**`bool PicoSend (PicoComms* M, const char* Str, bool CanWait=false)`**   :   Same as above, just a little simpler to use, if you have a c-string.

**`PicoMessage PicoGet (PicoComms* M, float TimeOut=0)`**   :   Gets a message if any exist. You can either return immediately if none are queued up, or wait for one to arrive.

    struct PicoMessage {
        int   Length;
        char* Data;
    };

This is what you get back. This gives you the `Length` of the data, and the `Data` itself. `Data` from `PicoGet`, is allocated with `malloc` and you must to `free` it after you are finished with it.

If you are a C++ expert you might try to find the C++ Spiders I have left in the code for you to discover! 🕸️ Don't worry they are friendly spiders.


### Utils

These functions are not always needed, but available in case you need them.

**`int PicoErr (PicoComms* M);`**   :   Returns an error that forced comms to close. If the comms is still open, the error is 0.

**`void PicoClose (PicoComms* M)`**   :   Closes the comms object. Does not destroy it. Useful if you have many places that might need to close the comms, but only one place that will destroy it. It acceptable to close a comms twice!

**`bool PicoStillSending (PicoComms* M)`**   :   Returns if the comms object is still in the business of sending. This is to let you keep your app open while busy sending.
    
**`void* PicoSay (PicoComms* M, const char* A, const char* B="", int Iter=0);`**   :   Prints a string to stdout. This can be used to debug or report things. This helpfully mentions if we are the parent or not, and also mentions our Comm's name. (`Name` is settable via `PicoConfig`).
    

### Configuration

Use the config function to get the config struct. **`PicoConfig* PicoConf (PicoComms* M)`** You can configure "noise", "timeout", "name", and the maximum unread-message queue size.


    struct PicoMessageConfig {
        const char* Name;
    /* For Reporting Events. If the parent comms name is "Helper",
    then on close you will see "Parent.Helper: Closed Gracefully" in stdout. */
        
        float       SendTimeOut;
    /* The number of seconds before a send will timeout
    (if the send is not instant). */

        int         QueueBytesRemaining;
    /* The allowed combined-size for unread messages. There is no hard limit,
    except the size of an int. Set it to 2GB if you want. 8MB default. */

        int         Noise;
	/* The amount of printing to StdOut that PicoMsg does.*/
    };

The `Noise` field, can be set to any of the below items. You can set it to silent, if you want PicoMsg to not be too "noisy" on StdOut. To set it to PicoNoiseAll to be the noisiest.

    PicoSilent
    PicoNoiseDebugChild	
    PicoNoiseDebugParent
    PicoNoiseDebug
    PicoNoiseEventsChild
    PicoNoiseEventsParent
    PicoNoiseEvents
    PicoNoiseAll        // combination of above


Please support this work, by donating. Or perhaps buying some copper jewelry which I am making these days. You won't regret it!

