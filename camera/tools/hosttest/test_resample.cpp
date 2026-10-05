#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <vector>
#include "../../arm9/source/audioresample.h"
static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static std::vector<int16_t> run(uint32_t rin, uint32_t rout, double hz, int seconds, double amp = 12000) {
	audioresample::Linear r;
	r.init(rin, rout);
	std::vector<int16_t> out;
	const int n = (int)rin * seconds;
	for (int i = 0; i < n; i++)
		r.push((int16_t)lrint(amp * sin(2 * M_PI * hz * i / rin)), [&](int16_t v) { out.push_back(v); });
	return out;
}
int main() {
	// identity: same rate, same samples
	{
		audioresample::Linear r;
		r.init(16000, 16000);
		std::vector<int16_t> out;
		for (int i = 0; i < 1000; i++) r.push((int16_t)(i * 7 - 3000), [&](int16_t v) { out.push_back(v); });
		CHECK(out.size() == 1000);
		bool same = true;
		for (int i = 0; i < 1000 && same; i++) same = out[i] == (int16_t)(i * 7 - 3000);
		CHECK(same);
	}
	// the DSi microphone rate (codec 32.728 kHz / 2) down to the 16 kHz container rate: 10 s
	{
		const auto out = run(16364, 16000, 440, 10);
		CHECK(out.size() >= 159990 && out.size() <= 160010); // the owner wants ~160,000 samples for 10 s
		// accuracy against the ideal sine at the output rate
		double worst = 0;
		for (size_t k = 100; k < out.size() - 100; k++) {
			const double ideal = 12000 * sin(2 * M_PI * 440 * ((double)k * 16364 / 16000) / 16364);
			worst = fmax(worst, fabs(ideal - out[k]));
		}
		CHECK(worst < 300); // linear interpolation error of a 440 Hz tone at 16 kHz
	}
	// 47.6 kHz codec: 23802 Hz down to 16000
	{
		const auto out = run(23802, 16000, 300, 10);
		CHECK(out.size() >= 159990 && out.size() <= 160010);
	}
	// upsampling 8000 -> 16000 doubles the count
	{
		const auto out = run(8000, 16000, 200, 2);
		CHECK(out.size() >= 31990 && out.size() <= 32010);
	}
	// no long-term drift: one hour of input gives the exact ratio to within one sample
	{
		audioresample::Linear r;
		r.init(16364, 16000);
		uint64_t outN = 0;
		const uint64_t inN = 16364ull * 3600;
		for (uint64_t i = 0; i < inN; i++) r.push(0, [&](int16_t) { outN++; });
		const double expect = (double)inN * 16000.0 / 16364.0;
		CHECK(fabs((double)outN - expect) < 2.0);
	}
	if (failures) { printf("%d failures\n", failures); return 1; }
	printf("test_resample OK\n");
	return 0;
}
