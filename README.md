# PicoMsg
**Miniature Message Passing IPC System (Single Header C++ File)**

### Licence:
https://creativecommons.org/licenses/by/4.0/ I chose this licence, because I need to be attributed.

### About
I created PicoMsg, because I couldn't find anything that fit my needs for a single-header, simple and fast message-passing library.

PicoMsg is thread-safe as long as you use the single-producer, single-consumer approach. That is, you can make two threads, and one thread can freely talk to the other using PicoMsg, as long as each thread only talks to it's own `PicoComms` object.

PicoMsg is simpler and smaller than nanomsg and zeromq, at around 450 SLOC.

PicoMsg uses two threads behind the scenes, to do read and writes. 


### Base API

These are the functions you need to use PicoMsg:

**`PicoComms* PicoMsgComms (int Flags=PicoNoiseEvents)`**   :   Creates your message-passer. You use this for inter-process-communications (IPC). You can get and send to this. Set Flags to 0 to make PicoMsg not print to stdout.

**`PicoComms* PicoMsgCommsPair (PicoComms* M, int Flags=PicoNoiseEvents)`**   :   Creates the second message-passer, and connects it to the first.

**`int PicoMsgFork (PicoComms* M)`**   :   This will fork your app, and then connect the two apps with PicoMsg.

**`void PicoMsgDestroy (PicoComms* M)`**   :   Destroys the PicoComms object. Accepts a `nil` PicoComms. (The others don't).

**`bool PicoMsgSend (PicoComms* M, PicoMessage Msg, float Timeout=0)`**   :   Sends the message. The data is copied to internal buffers so you do not need to hold onto it after send. `TimeOut` is in seconds.
                                                                                                                                                
**`bool PicoMsgSend (PicoComms* M, const char* Str, float Timeout=0)`**   :   Same as above, just a little simpler to use, if you have a c-string.

**`PicoMessage PicoMsgGet (PicoComms* M, float TimeOut=0)`**   :   Gets a message if any exist. You can either return immediately if none are queued up, or wait for one to arrive.

    struct PicoMessage {
        char* Data;
        int   Length;
    };

This is what you get back. This gives you the `Length` of the data, and the `Data` itself. `Data` from `PicoMsgGet` is, allocated with `malloc` and you must to `free` it after you are finished with it.

If you are a C++ expert you might try to find the C++ Spiders I have left in the code for you to discover! 🕸️ Don't worry they are friendly spiders.


### Utils

These functions are not always needed, but available in case you need them.

**`int PicoMsgErr (PicoComms* M);`**   :   Returns an error that forced comms to close. If the comms is still open, the error is 0.

**`void PicoMsgClose (PicoComms* M)`**   :   Closes the comms object. Does not destroy it. Useful if you have many places that might need to close the comms, but only one place that will destroy it. It acceptable to close a comms twice!

**`bool PicoMsgStillSending (PicoComms* M)`**   :   Returns if the comms object is still in the business of sending. In this case you might not want to keep your app open while it is still sending.
    
**`void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0);`**   :   Prints a string to stdout. This can be used to debug or report things. This helpfully mentions if we are the parent or not, and also mentions our Comm's name. (`Name` is settable via PicoMsgConfig).
    
**`PicoConfig* PicoMsgConf (PicoComms* M)`**    :   Gets the config struct. This lets you configure how your comms will work. Like noise, timeouts, name, and maximum unread-message queue size.

    struct PicoMessageConfig {
        const char* Name;                // For Reporting Events. If the parent comms name is "Helper", then on close you will see "Parent.Helper: Closed Gracefully" in stdout.
        int         Noise;
        float       SendTimeOut;         // The number of seconds before a send will tiemout, (if the send is not instant).
        int         QueueBytesRemaining; // The allowed size for unread messages that are queued up for you.
    };

The `Noise` value of the config, can be set to any of the below items. It can also be set on creating the comms object using PicoMsgComms.

    PicoSilent
    PicoNoiseDebugChild	
    PicoNoiseDebugParent
    PicoNoiseDebug
    PicoNoiseEventsChild
    PicoNoiseEventsParent
    PicoNoiseEvents
    PicoNoiseAll        // combination of above
    

###

Please support this work, by donating. Or perhaps buying some copper jewelry which I am making these days. You won't regret it!

