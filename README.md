# PicoMsg
**Miniature Message Passing IPC System (Single Header C++ File)**


### Licence:
https://creativecommons.org/licenses/by/4.0/ I chose this licence, for it's strict requirement of attribution, while still being quite permissive.


### About
PicoMsg is a single-header, thread-safe, simple and fast message-passing library.

PicoMsg uses the single-producer, single-consumer approach. PicoMsg is simpler and smaller than nanomsg and zeromq, at around 500 SLOC.

PicoMsg uses a worker thread behind the scenes, to read and write. PicoMsg will communicate with sockets across processes. But if you are using PicoMsg to communicate within a process, it uses direct memory sharing! Much faster! You can also configure PicoMsg, like having multiple worker-threads, or changing how much memory it uses.


### Usage

PicoMsg is almost always non-blocking. The default buffer sizes are: Send=1MB, Receive=1MB. The received message queue is allocated with malloc, and maxes at 8MB unread messages. If your program is busy sending a lot of data, it probably won't block.

The other side, will have two worker-threads that slurp up all your data. One thing to remember, is that you can't send messages bigger than your buffers. That limits us to 1MB-4 bytes per message, by default. PicoMsg will send and get multiple messages per read/send event, if multiple are available.

If the default behaviour doesn't work for you, feel free to tweak it! You can specify the buffer size, by passing your size to `PicoCreate (const char* Name, int BufferByteSize)`. A size of 0, defaults to 1MB. The queue defaults to 8x the buffer size.


### Building

PicoMsg is best placed at `/usr/local/include/PicoMsg/PicoMsg.h`. That way, all your projects can include it! But will work fine no matter where it is placed.

	cd /usr/local/include/PicoMsg/
	g++ PicoTest.cpp -o picotest -std=c++20 -Os

Then you can run the executable using "`picotest 1`" or "`picotest 2`" or "`picotest 3`".


# API

The API is mostly [described in PicoMsg.h itself](PicoMsg.h). However, a quick explanation is here:

Start by calling `PicoCreate`, then call either `PicoStartChild`, `PicoStartThread` or `PicoStartFork` on your `PicoComms*`. Call `PicoDestroy` when you are finished.

To send, use `PicoSend`. To receive, use `PicoGet`. Both calls are non-blocking by default, but you can ask to block within the function call itself.

Theres also helper functions, like `PicoSendStr` (sends c-strings), or PicoGet2, which allows C++ style gets.

If you are a C++ expert you might try to find the C++ Spiders I have left in the code for you to discover! 🕸️ Don't worry they are friendly spiders.

PicoMsg also has some util functions. These functions are not always needed, but available in case you need them.

`PicoError` is very nice, because it returns an error that forced comms to close. If the comms is still open, the error is 0. The errors are from `errno`, so you can use `strerror` on them. For example, a PicoComms that was newly created, will have an error of `ENOTCONN`, meaning that it is not (yet) connected :) This error will go to 0, once you connect the comms.

Other useful utils are: `PicoClose` (in case you want to close the comms from multiple points in your app), `PicoStillSending` (in case you want to give your app a chance to still send more data.), `PicoSay` is very informative and can help debug things.

`PicoConfig` is useful to configure things about PicoMsg, such as the timeout-value, maximum unread-message queue size, and some variables used to improve (or disable) error-reporting to stdout.


Please support this work, by donating. Or perhaps buying some copper jewelry which I am making these days. You won't regret it!

