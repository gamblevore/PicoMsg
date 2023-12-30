# picomsg

Miniature Message Passing for IPC - Single Header C++


I made picomsg, because I couldn't find anything that fit my needs for a single-header, simple and fast message-passing library.

Simpler and smaller than nanomsg and zeromq.

PicoMsg is so small it could be part of the standard unix distribution... included into the StdCLib and Linus himself will give me $1000001!

     🥰🥰  🥰🥰
    🤭😂🤣😢😢😢           💰💰💰💰💰
     🫢🪦💅🤗👀     -->  💰🤑💎💍🫢👍😇
       🥹🥹🥹
         🥰

PicoMsg expects to only be called from one thread at a time. Calling it across threads is no problem, as long as previously called functions on other threads, have finished.

### Warning
This is a very early, and untested beta. I guess I will update this note in some months time after it is successfully working and not causing any problems. 

### Base API

These are the functions you need to use PicoMsg:

| Function                                                    | Description                                                                                                                                                         |
|-------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| PicoComms* PicoMsg (int Flags=PicoNoisy)                    | Creates your message-passer. You use this for inter-process-communications (IPC). You can get and send to this. Set Flags to 0 to make PicoMsg not print to stdout. |
| int PicoMsgFork (PicoComms* M)                              | This will fork your app, and then connect the two apps with PicoMsg.                                                                                                |
| void PicoMsgDestroy (PicoComms* M)                          | Destroys the PicoComms object. Accepts a `nil` PicoComms. (The others don't).                                                                                       |
| bool PicoMsgSend (PicoComms* M, const void* data, int n=-1) | unimplemented                                                                                                                                                       |
| PicoMessage PicoMsgGet (PicoComms* M, float TimeOut=0)      | Gets a message if any exist. You can either return immediately if none are queued up, or wait for one to arrive.                                                    |

    struct PicoMessage {
        int Remain;
        int Length;
        char* Data;
    };

This is what you get back. Ignore the "`Remain`" field you don't need it. This gives you the `Length` of the data, and the `Data` itself. `Data` is allocated with `malloc` and you have to `free` it after.


### Utils

These functions are not always needed, but available in case you need them.

`int PicoMsgErr (PicoComms* M);`    Gets back an error, if any occurred that forced the thing to close. If the comms is still open, then the error is 0.
    
`void* PicoMsgSay (PicoComms* M, const char* A, const char* B="", int Iter=0, bool Strong=true);`    Prints a string to stdout. This can be used to debug or report things. This helpfully mentions if we are the parent or not, and also mentions our Comm's name.
    
`PicoMessageConfig* PicoMsgConfig (PicoComms* M);`    Gets the config object! Now you can do useful things like set PicoMsg's name, which it uses during error-reporting.

    struct PicoMessageConfig {
        const char* Name;
        int         MaxSend; // refuses to send or receive larger messages.
        int         Flags;
    };

Flags can be these:

    PicoNoisyChild
    PicoNoisyParent
    PicoNoisy               // combination of above


###

Please support this work, by donating. Or perhaps buying some copper jewelry which I am making these days. You won't regret it!

