#include "../../src/RemixCameraMath.h"
#include <array>
#include <cmath>
#include <iostream>

int main()
{
	double worst = 0;
	unsigned samples = 0;
	for (float yaw : { 0.0f, 0.3f, 1.57f, 3.2f, 5.8f }) {
		for (float pitch : { -1.2f, 0.0f, 0.7f }) {
			const float cy = std::cos(yaw), sy = std::sin(yaw);
			const float cp = std::cos(pitch), sp = std::sin(pitch);
			// Row-vector yaw/pitch rotation with a nonzero relative eye offset.
			const std::array<float, 16> relative{
				cy, sy * sp, sy * cp, 0,
				0, cp, -sp, 0,
				-sy, cy * sp, cy * cp, 0,
				2.5f, -1.25f, 0.75f, 1
			};
			for (const auto& origin : { std::array<float,3>{0,0,0},
				std::array<float,3>{17224.77f,-47204.45f,30},
				std::array<float,3>{-200000,300000,7000} }) {
				auto absolute = relative;
				RemixCameraMath::RestoreWorldViewTranslation(absolute.data(), origin.data());
				// First-person nodes are restored by a uniform translation after
				// their native pass. Moving the captured camera origin by that
				// translation must preserve view-space positions of those nodes.
				for (const auto& restoredDelta : { std::array<float, 3>{0, 0, 0},
					std::array<float, 3>{-17224.77f, 47204.45f, -30},
					std::array<float, 3>{700, -200, 121} }) {
					auto restoredView = relative;
					std::array<float, 3> restoredOrigin{};
					for (unsigned row = 0; row < 3; ++row)
						restoredOrigin[row] = origin[row] + restoredDelta[row];
					RemixCameraMath::RestoreWorldViewTranslation(restoredView.data(), restoredOrigin.data());
					for (unsigned col = 0; col < 4; ++col) {
						double native = relative[12 + col], restored = restoredView[12 + col];
						for (unsigned row = 0; row < 3; ++row) {
							const double vertex = 15.0 * (row + 1);
							native += (vertex - origin[row]) * relative[row * 4 + col];
							restored += (vertex + restoredDelta[row]) * restoredView[row * 4 + col];
						}
						const auto error = std::abs(native - restored);
						worst = std::max(worst, error);
						if (error > 0.04) {
							std::cerr << "View-model restoration mismatch " << error << '\n';
							return 1;
						}
						++samples;
					}
				}
				for (const auto& offset : { std::array<double,3>{0,0,0},
					std::array<double,3>{300,-200,80}, std::array<double,3>{-1000,100,2000} }) {
					for (unsigned col=0; col<4; ++col) {
						double a=relative[12+col], b=absolute[12+col];
						for (unsigned row=0;row<3;++row) {
							a += offset[row] * relative[row*4+col];
							b += (origin[row]+offset[row]) * absolute[row*4+col];
						}
						const double error=std::abs(a-b);
						worst=std::max(worst,error);
						if(error>0.04) {
							std::cerr << "World-view mismatch " << error << '\n';
							return 1;
						}
						++samples;
					}
				}
			}
		}
	}
	std::cout << samples << " coordinate comparisons passed, max error " << worst
		<< " game units (float matrix translation). Not a renderer/temporal test.\n";
}
