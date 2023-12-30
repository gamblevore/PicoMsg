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

