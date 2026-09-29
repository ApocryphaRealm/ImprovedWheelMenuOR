#pragma once

// The TestBench driving tool (rule 64): iwm.pad - presses the controller for a test, and reads back the wheels.
namespace tool
{
	bool Register();   // lazily, once TestBench.dll is loaded (idempotent)
	void Pump();       // every controller read, on the game thread: answers a pending state request
}
