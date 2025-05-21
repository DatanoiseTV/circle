// sample/44-multicasttest/main.cpp
#include "kernel.h"

extern "C" // Program entry point
{
	int main (void)
	{
		CKernel Kernel;
		if (!Kernel.Initialize ())
		{
			return -1; // Initialization failed
		}

		return Kernel.Run ();
	}
}
