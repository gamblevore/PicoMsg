
// todo: fool around with error handling
// handle bigger data
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


int TestWrite(char* Out, int i, char Base) {
	int j = 0;
	for (int reps = 0; reps <= i % 3; reps++) {
		int x = i;
		while (j < 16) {
			int y = x % 26;
			x /= 26;
			Out[j++] = y + 'A' + Base;
			if (x == 0)
				break;
		}
	}
	Out[j] = 0;
	return j+1;
}

int RecIndex = 0;
bool TestIntenseCompare (PicoComms* C, float T) {
	char Expected[20] = {};
	auto Rec = PicoMsgGet(C, T); if (!Rec) return false;
	auto Found = Rec.Data;
	TestWrite(Expected, RecIndex, 0);
	printf("<-- %s %i\n", Found, RecIndex);
	bool Rz = strcmp(Expected, Found) == 0;
	if (!Rz) {
		printf("String %i differed! Expected '%s' but found '%s'\n", RecIndex, Expected, Found);
		T = T;
	}
	RecIndex++;
	free(Found);
	return Rz;
}
// --> HFLHFL 7574

int TestIntense (PicoComms* C) {
	int PID = PicoMsgFork(C);
	C->Conf.Noise = PicoNoiseDebug;
	if (PID < 0)
		return -PID;
	if (!PID) {
		C->Conf.Name = "Tester";
		char Out[20] = {}; memset(Out, -1, sizeof(Out));
		PicoMessage Snd = {Out};
		PicoMsgSay(C, "Asks intensely");
		for (int i = 0; i < 100000; i++) {
			Snd.Length = TestWrite(Out, i, 1);
			if (!PicoMsgSend(C, Snd)) {
//				sleep(1); // not sure why the other side does run at the same time?
				if (!PicoMsgSend(C, Snd))
					return puts("Exitting Sadly") | -1;
			}
			printf("--> %s %i\n", Snd.Data, i);
			TestIntenseCompare(C, 0);
		}
		while (TestIntenseCompare(C, 10)) {;}
		printf("%i strings compared.\n", RecIndex);
		sleep(1);

	} else {
		C->Conf.Name = "Fixer";
		int Back = 0;
		char Out[20] = {}; memset(Out,-1,sizeof(Out));
		while (auto Msg = PicoMsgGet(C, 10.0)) {
			auto D = Msg.Data; Msg.Data = Out;
			int n = Msg.Length-1;
			for (int j = 0; j < n; j++)
				Out[j] = D[j] - 1;
			Out[n] = 0;
			printf("    %i: %s(%i) >> %s\n", Back, D, n, Out);
			PicoMsgSend(C, Msg);
			free(D);
			Back++;
			if (PicoMsgErr(C)) break;
		}
		while (PicoMsgStillSending(C))
			sleep(1);
	}
	PicoMsgSay(C, "Finished");
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

