
// todo: fool around with error handling
// handle bigger data, and picobuffer rewraps
// have a block on send feature...
#define PICO_IMPLEMENTATION
#include "PicoMsg.h"
#include <unistd.h>
#include <vector>
using std::vector;


void* Query (PicoComms* M) {
	M->Conf.Name = "Query";
	PicoMsgSendStr(M, "mary had a little lamb");
	const int Stack = 10; const int Pudge = 4096;
	vector<char> abcd(Stack*Pudge);
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] = rand();
	}
	
	vector<PicoMessage> Sent;
	char* abc = &abcd[0];
	int Remain = Stack*Pudge;
	while (Remain > 0) {
		if (PicoMsgErr(M)) return 0;

		int n = rand() % Pudge;
		if (n > Remain) n = Remain;
		Remain -= n;
		Sent.push_back({abc, n});
		PicoMsgSend(M, {abc, n});
		abc += n;
	}
	
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] ++;
	}

	PicoMsgSay(M, "Comparing", "", (int)Sent.size());

	for (int i = 0; i < Sent.size(); i++) {
		auto& v = Sent[i];
		auto OK = PicoMsgGet(M, 7.0);
		if (!OK)								return 0;
		if (OK.Length != v.Length)				return PicoMsgSay(M, "bad length");
		int diff = memcmp(v.Data, OK.Data, v.Length);
		if (diff)								return PicoMsgSay(M, "netdown");
		abc += v.Length;
		PicoMsgSay(M, "Passed", "", i+1);
		free(OK.Data);
	}
	PicoMsgSay(M, "Tests Passed!");
	return 0;
}


void Respond (PicoComms* M) {
	M->Conf.Name = "Respond";
	auto Mary = PicoMsgGet(M, 6.0);
	if (Mary) {
		PicoMsgSay(M, "Received", Mary.Data);
		free(Mary.Data);
	} else {
		PicoMsgSay(M, "Missed Data");
	}
	
	while (!PicoMsgErr(M)) {
		auto Msg = PicoMsgGet(M, 2);
		if (!Msg) break;

		for (int i = 0; i < Msg.Length; i++) {
			(Msg.Data)[i]++;
		}
		PicoMsgSend(M, Msg);
		free(Msg.Data);
	}
	M->SayEvent( "End" );
}


int TestPair (PicoComms* C) {
	auto C2 = PicoMsgCommsPair(C, PicoNoiseEvents);
	if (C2) {
		PicoMsgSendStr(C, "pear🍐🍐🍐test");
		auto Msg = PicoMsgGet(C2, 2.0);
		if (Msg.Data)
			PicoMsgSay(C2, "Received:", Msg.Data);
		  else
			PicoMsgSay(C2, "failed get");
		free(Msg.Data); // always free what you get back!
	}
	PicoMsgDestroy(C2);
	return 0;	
}


int TestIntense (PicoComms* C) {
	int PID = PicoMsgFork(C);
	if (PID < 0)
		return -PID;
	if (PID) {
		char Data[20] = {};
		for (int iters = 1; iters <= 10; iters++) {
			PicoMessage Snd = {Data};
			for (int i = 0; i <= 1000; i++) {
				int j = 0;
				for (int reps = 0; reps <= i % 3; reps++) {
					int x = i;
					for (; j < 16; j++) {
						int y = x % 26;
						x /= 26;
						Data[j] = y + 'B';
						if (x == 0) {
							j++;
							Data[j] = 1;
							Snd.Length = j;
							break;
						}
					}
				}
				PicoMsgSend(C, Snd);
				auto Rec = PicoMsgGet(C, 0);
				if (Rec)
					puts(Rec.Data);
				free(Rec.Data);
			}
		}
		
		while (1) {
			auto Rec = PicoMsgGet(C, 1);
			if (!Rec) break; 
			puts(Rec.Data);
			free(Rec.Data);
		}
		sleep(1);

	} else {
		while (auto Msg = PicoMsgGet(C, 3.0)) {
			for (int j = 0; j < Msg.Length; j++)
				Msg.Data[j]--;
			PicoMsgSend(C, Msg);
			free(Msg.Data);
		}
		while (PicoMsgStillSending(C))
			sleep(1);
	}
	return 0;	
}


int TestThread (PicoComms* C) {
	int PID = PicoMsgFork(C);
	if (PID < 0)
		return -PID;
	if (PID)
		Query(C);
	  else
		Respond(C);
	if (PID) sleep(4); // let the child exit
	return 0;	
}


int main (int argc, const char * argv[]) {
	auto C = PicoMsgComms();
	int rz = 0;
	if (true or strcmp(argv[0], "intense")==0)
		rz = TestIntense(C);
	 else if (strcmp(argv[0], "pair")==0)
		rz = TestPair(C);
	 else
		rz = TestThread(C);
	PicoMsgDestroy(C);
	return rz;
}

