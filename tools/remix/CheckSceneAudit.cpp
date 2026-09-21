#include "../../src/RemixSceneAudit.h"
#include <cassert>
#include <iostream>

int main()
{
	RemixSceneAudit::History<4> audit;
	RemixSceneAudit::Sample sample;
	sample.cell = 1;
	sample.ready = true;
	sample.classes[2] = 3;
	audit.Record(sample);
	assert(!audit.firstSuspect);
	sample.cell = 2;
	sample.interior = true;
	sample.frame = 1;
	audit.Record(sample); // Exactly one bad transition frame.
	sample.classes = {};
	for (uint64_t i = 2; i != 12; ++i) {
		sample.frame = i;
		audit.Record(sample);
	}
	assert(audit.samples == 12 && audit.interiorSamples == 11);
	assert(audit.cellChanges == 1 && audit.suspectSamples == 1);
	assert(audit.interiorMax[2] == 3 && audit.exteriorMax[2] == 3);
	assert(audit.firstSuspect && audit.firstSuspect->frame == 1);
	assert(audit.Size() == 4 && audit.Oldest(0).frame == 8 && audit.Oldest(3).frame == 11);
	sample.dome = true;
	audit.Record(sample);
	sample.dome = false;
	sample.interior = false;
	sample.classes[6] = 1;
	sample.ready = false;
	audit.Record(sample);
	assert(audit.suspectSamples == 3 && audit.failedSamples == 1);
	assert(audit.firstFailure && !audit.firstFailure->ready && audit.missingCameraSamples == audit.samples);
	sample.cameraCurrent = true;
	audit.Record(sample);
	assert(audit.cameraReadyFailures == 1 && audit.failedSamples == 2);
	assert(audit.interiorDomeSamples == 1 && audit.exteriorDomeSamples == 0);
	audit = {};
	assert(!audit.first && !audit.firstSuspect && audit.samples == 0 && audit.Size() == 0);
	std::cout << "Scene audit: single-frame preservation, rollover, classification and reset passed.\n";
}
