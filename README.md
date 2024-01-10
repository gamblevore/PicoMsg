# PicoMsg
**Miniature Message Passing IPC System (Single Header C++ File)**

### Licence:
https://creativecommons.org/licenses/by/4.0/ I chose this licence, for it's strict requirement of attribution, while still being quite permissive.

### About
I created PicoMsg, because I couldn't find anything that fit my needs for a single-header, simple and fast message-passing library.

PicoMsg is thread-safe as long as you use the single-producer, single-consumer approach. That is, you can make two threads, and one thread can freely talk to the other using PicoMsg, as long as each thread only talks to it's own `PicoComms` object.

PicoMsg is simpler and smaller than nanomsg and zeromq, at around 450 SLOC.

PicoMsg uses two threads behind the scenes, to do read and writes. 

### Usage

Generally, PicoMsg tries very hard to be non-blocking. That is, we have a send buffer, a receive buffer, and a message queue! Our buffers default: send 1MB, receive 1MB, queue up to 8MB (it grows). So if your program is busy sending a lot of data, it probably won't block.

The "queue" is added to, automatically everytime a message is completely received. The other side probably will be slurping up the data, even if your main thread is busy doing something else. However, you can't send messages bigger than your send buffer, or other side's receive buffer. So by default, that limits us to 1MB-4 bytes of data, per message.

If your main thread is very quickly creating a lot of very small messages, your send buffer could contain (let's say) 1 thousand messages, before our worker-threads see the new data. Then PicoMsg will send as many as possible, which might be all 1 thousand! So the receiving program could easily have 1 thousand messages placed in it's message-queue at once. In fact, this makes PicoMsg much faster than if it were acting like "one in, one out". It is designed to be as fast as possible.

PicoMsg is very open source, so if the default behaviour is not good enough for you, feel free to tweak it! It shouldn't be hard to make the buffer-size growable if you really need that... or default to lower-sizes if you prefer.

### Building

	cd /Path/To/Proj
	g++ PicoTest.cpp -o picotest -std=c++20 -Os

Then you can run the executable using "`picotest 1`" or "`picotest 2`" or "`picotest 3`".

# API

### Initialisation / Destruction

Start by calling `PicoMsgComms`, then call either `PicoMsgCommsChild`, `PicoMsgThread` or `PicoMsgFork` on it. Call `PicoMsgDestroy` when you are done with any `PicoComms*`.

**`PicoComms* PicoMsgComms ()`**   :   Creates your message-passer.

**`PicoComms* PicoMsgCommsChild (PicoComms* M)`**   :   Creates a child message-passer, and connects it to the parent. Only needed for threading, not forks.

**`int PicoMsgThread (PicoComms* M, PicoThreadFn fn)`**   :   Creates a new thread, using the function "fn", and passes a new PicoComms object to it! A handier way to run PicoMsg. Check the PicoTest.cpp file for an example. 

**`int PicoMsgFork (PicoComms* M)`**   :   This will fork your app, and then connect the two apps with PicoMsg.

**`void PicoMsgDestroy (PicoComms* M)`**   :   Destroys the PicoComms object. Accepts a `nil` PicoComms. (The others don't). Destroying one side does not destroy the other, will also need PicoMsgDestroy called on it. But destroying one side does close the other, at least. So sends will be ignored.

### Communication

**`bool PicoMsgSend (PicoComms* M, PicoMessage Msg, bool CanWait=false)`**   :   Sends the message. The data is copied to internal buffers so you do not need to hold onto it after send. If `CanWait` is false and there is no buffer space, this function returns `false`. If `CanWait` is true, it will block until the timeout is reached. See the ["configuration"](#Configuration) section about how to change the timeout.

**`bool PicoMsgSend (PicoComms* M, const char* Str, bool CanWait=false)`**   :   Same as above, just a little simpler to use, if you have a c-string.

**`PicoMessage PicoMsgGet (PicoComms* M, float TimeOut=0)`**   :   Gets a message if any exist. You can either return immediately if none are queued up, or wait for one to arrive.

    struct PicoMessage {
        int   Length;
        char* Data;
    };

This is what you get back. This gives you the `Length` of the data, and the `Data` itself. `Data` from `PicoMsgGet` is, allocated with `malloc` and you must to `free` it after you are finished with it.

If you are a C++ expert you might try to find the C++ Spiders I have left in the code for you to discover! 🕸️ Don't worry they are friendly spiders.


### Utils

These functions are not always needed, but available in case you need them.

**`int PicoMsgErr (PicoComms* M);`**   :   Returns an error that forced comms to close. If the comms is still open, the error is 0.

**`void PicoMsgClose (PicoComms* M)`**   :   Closes the comms object. Does not destroy it. Useful if you have many places that might need to close the comms, but only one place that will destroy it. It acceptable to close a comms twice!

**`bool PicoMsgStillSending (PicoComms* M)`**   :   Returns if the comms object is still in the business of sending. This is to let you keep your app open while busy sending.
    
**`void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0);`**   :   Prints a string to stdout. This can be used to debug or report things. This helpfully mentions if we are the parent or not, and also mentions our Comm's name. (`Name` is settable via PicoMsgConfig).
    

### Configuration

Use the config function to get the config struct. **`PicoConfig* PicoMsgConf (PicoComms* M)`** You can configure "noise", "timeout", "name", and the maximum unread-message queue size.


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

The `Noise` field, can be set to any of the below items. It can also be set on creating the comms object using PicoMsgComms. You can set it to silent, if you want PicoMsg to not be too "noisy" on StdOut. To set it to PicoNoiseAll to be the noisiest.

    PicoSilent
    PicoNoiseDebugChild	
    PicoNoiseDebugParent
    PicoNoiseDebug
    PicoNoiseEventsChild
    PicoNoiseEventsParent
    PicoNoiseEvents
    PicoNoiseAll        // combination of above


Please support this work, by donating. Or perhaps buying some copper jewelry which I am making these days. You won't regret it!

