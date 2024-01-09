
// can we remove the sleep during read?
// fix the warn-after logic!
// make the list own it

#define PICO_IMPLEMENTATION
#include "PicoMsg.h"
#include <unistd.h>
#include <vector>
using std::vector;

uint hash (uint x) {
	x ^= x >> 16;
	x *= UINT32_C(0x7feb352d);
	x ^= x >> 15;
	x *= UINT32_C(0x846ca68b);
	x ^= x >> 16;
	return x;
}


void* Query (PicoComms* M) {
	M->Conf.Name = "Query";
	M->Conf.Noise = PicoNoiseAll;
	PicoMsgSendStr(M, "mary had a little lamb");
	const int Stack = 17; const int Pudge = 4096;
	vector<char> abcd(Stack*Pudge);
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] = (char)(hash(i)%26)+'A';
	}
	
	vector<PicoMessage> Sent;
	vector<PicoMessage> Got;
	char* abc = &abcd[0];
	int Remain = Stack*Pudge;
	while (Remain > 0) {
		if (PicoMsgErr(M)) return PicoMsgSay(M, "Exit: CantSendAll");

		int n = hash(Remain) % Pudge;
		if (n > Remain) n = Remain;
		if (n < 1) n = 1;
		Remain -= n;
		Sent.push_back({abc, n});
		PicoMsgSend(M, {abc, n}, PicoSendCanTimeOut);
		auto Back = PicoMsgGet(M);
		if (Back)
			Got.push_back(Back);
		abc += n;
	}
	
	for (int i = 0; i < Stack*Pudge; i++) {
		abcd[i] ++;
	}

	PicoMsgSay(M, "Comparing", "", (int)Sent.size());
	
	while (auto Back = PicoMsgGet(M, 7*(Sent.size() > Got.size()))) {
		Got.push_back(Back);
	}

	if (int Diff = (int)(Sent.size() - Got.size()); Diff) 
		return PicoMsgSay(M, "Missing Responses: ", "", Diff);

	for (int i = 0; i < Sent.size(); i++) {
		auto& v = Sent[i];
		auto& OK = Got[i];
		if (OK.Length != v.Length)				return PicoMsgSay(M, "Exit: BadLength");
		int diff = memcmp(v.Data, OK.Data, v.Length);
		if (diff)								return PicoMsgSay(M, "Exit: BadContents");
		abc += v.Length;
		PicoMsgSay(M, "Passed", "", i+1);
		free(OK.Data);
	}
	PicoMsgClose(M);
	PicoMsgSay(M, "Exit: Tests Passed!");
	return 0;
}


void Respond (PicoComms* M) {
	M->Conf.Name = "Respond";
	M->Conf.Noise = PicoNoiseAll;
	auto Mary = PicoMsgGet(M, 6.0);
	if (!Mary) return;
	PicoMsgSay(M, "WasAsked", Mary.Data);
	free(Mary.Data);
	
	int n = 0;
	while (!PicoMsgErr(M)) {
		auto Msg = PicoMsgGet(M, 2);
		if (!Msg) break;

		for (int i = 0; i < Msg.Length; i++) {
			(Msg.Data)[i]++;
		}
		PicoMsgSend(M, Msg, PicoSendCanTimeOut);
		free(Msg.Data);
		n++;
	}
	PicoMsgSay(M, "Responses Given:", "", n);
}


int TestPair (PicoComms* C) {
	auto C2 = PicoMsgCommsPair(C, PicoNoiseEvents);
	if (C2) {
		PicoMsgSendStr(C, "pear🍐🍐🍐test");
		auto Msg = PicoMsgGet(C2, 2.0);
		if (Msg.Data)
			PicoMsgSay(C2, "WasAsked", Msg.Data);
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


int TestIntense (PicoComms* C) {
	int PID = PicoMsgFork(C);
	C->Conf.Noise = PicoNoiseAll;
	if (PID < 0)
		return -PID;
	if (PID) {
		C->Conf.Name = "Tester";
		char Out[20] = {}; memset(Out, -1, sizeof(Out));
		PicoMessage Snd = {Out};
		PicoMsgSay(C, "Asks intensely");
		for (int i = 0; i < 100000; i++) {
			Snd.Length = TestWrite(Out, i, 1);
			if (!PicoMsgSend(C, Snd)) {
				return !PicoMsgSay(C, "Exitting Sadly");
			}
			PicoMsgSay(C, "Sending", Snd.Data, i);
			TestIntenseCompare(C, 0);
		}
		PicoMsgSay(C, "AllSent!");
		while (TestIntenseCompare(C, 10)) {;}
		PicoMsgSay(C, "strings compared", "", RecIndex);
		sleep(1);
		// OK... so how do I solve this?
		// it seems like its ending after a certain amount of time
		// with no explaination why. the "Fixer" side is being blocked to read
		// that means... noting is being sent. Is that true? And why would
		// that happen? Did we quit???? DID WE?

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
//			printf("    %i: %s(%i) >> %s\n", Back, D, n, Out);
			PicoMsgSend(C, Msg);
			free(D);
			Back++;
			if (PicoMsgErr(C)) break;
		}
		PicoMsgSay(C, "NoMoreInputs", "", Back);
		
		while (PicoMsgStillSending(C))
			sleep(1);
	}
	
	PicoMsgSay(C, "Acheived");
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


void TestBuffers() {
	PicoBuff B;
	B.Alloc(5, "🕷️"); // we need a lot of spiddles for this.
	// like a lot.
	for (int i = 0; i < 6; i++)
		B.append_sub("hello", 5);
	B.lost(10);
	B.append_sub("biggoodbye", 10);
	B.lost(5);
	B.append_sub("hugs", 4);
}


int main (int argc, const char * argv[]) {
	TestBuffers();
	auto C = PicoMsgComms();
	int rz = 0;
	argv[0] = "3";
	if (strcmp(argv[0], "1")==0)
		rz = TestIntense(C);
	 else if (strcmp(argv[0], "2")==0)
		rz = TestPair(C);
	 else
		rz = TestThread(C);
	PicoMsgDestroy(C);
	return rz;
}

