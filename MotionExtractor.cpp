#include "precomp.hpp"
#include "MotionExtractor.hpp"

#include "Exceptions.hpp"
#include "MKMath.hpp"
#include "VideoFrame.hpp"

using namespace std;

namespace {

const size_t kDownscaleRatio = 2;
const size_t kDownscaleSquare = kDownscaleRatio * kDownscaleRatio;
const size_t kBytesPerPixel = 3;

} // end anonymous namespace


MotionExtractor::MotionExtractor(size_t frameWidth,
                                 size_t frameHeight,
                                 double videoFPS,
                                 bool benchmark)
	: motionMask(),
	  fps(videoFPS),
	  motionThreshold(26),
	  stableCap((unsigned int)ceil(videoFPS)), // Make the stable cap equal to one second of frames
	  erosionLevel(5),
	  erodedMask(),
	  offs(),
	  currentImage(),
	  currentStableTimes(),
	  downscaleBuff(),
	  downscaleAccumulators(),
	  refImage(),
	  stableRecords(),
	  firstFrame(true),
	  imageWidth(0),
	  imageHeight(0),
	  imageArea(0),
	  imageSize(0),
	  srcLineSize(0),
	  destLineSize(0),
	  benchmarking(benchmark),
	  lastMark(clock()),
	  detectorFPS(0),
	  framesCounted(0)
	  // Some of these aren't necessary, but appease g++ -Weffc++
{
	// We're going to downscale the image by a certain ratio to speed up
	// analysis and reduce the impact of noise
	imageWidth = frameWidth / kDownscaleRatio;
	imageHeight = frameHeight / kDownscaleRatio;
	srcLineSize = frameWidth * kBytesPerPixel;
	destLineSize = imageWidth * kBytesPerPixel;

	// Light up our buffers
	imageArea = imageWidth * imageHeight;
	imageSize = imageArea * kBytesPerPixel;
	currentImage.reset(new VideoFrame(imageWidth, imageHeight, kBytesPerPixel, false));
	currentStableTimes.resize(imageArea);
	downscaleBuff.reset(new VideoFrame(imageWidth, imageHeight, kBytesPerPixel));
	downscaleAccumulators.resize(imageSize);
	refImage.reset(new VideoFrame(imageWidth, imageHeight, kBytesPerPixel, false));
	stableRecords = new unsigned int[imageArea];
	motionMask.reset(new VideoFrame(imageWidth, imageHeight, kBytesPerPixel, false));
	erodedMask.reset(new VideoFrame(imageWidth, imageHeight, kBytesPerPixel, false));

	// Generate pixel offsets for erosion
	const int po = (int)motionMask->getBytesPerPixel(); // One pixel's worth of offset
	const int upOne = -(int)(motionMask->getBytesPerPixel() * motionMask->getWidth()); // The offset for the pixel above the current one
	offs.push_back(PixelOffset(-1, 0, -po));
	offs.push_back(PixelOffset(1, 0, po));
	offs.push_back(PixelOffset(-1, -1, upOne - po));
	offs.push_back(PixelOffset(0, -1, upOne));
	offs.push_back(PixelOffset(1, -1, upOne + po));
	offs.push_back(PixelOffset(-1, 1, upOne - po));
	offs.push_back(PixelOffset(0, 1, upOne));
	offs.push_back(PixelOffset(1, 1, upOne + po));

	// Initialize our time-dependant stuff to the desired initial state
	reset();
}

/// Returns true if the two pixels are significantly different
bool MotionExtractor::pixelIsDifferent(uint8_t* __restrict pa,
                                       uint8_t* __restrict pb)
{
	bool ret = false;
	for (size_t b = 0; b < kBytesPerPixel; ++b) {
		if (abs((int)pa[b] - (int)pb[b]) > motionThreshold) {
			ret = true;
			break;
		}
	}
	return ret;
}

VideoFrame& MotionExtractor::generateMotionMask(const VideoFrame& frame)
{
	if (benchmarking) {
		if (clock() > lastMark + CLOCKS_PER_SEC) {
			detectorFPS = framesCounted;
			framesCounted = 0;
			lastMark = clock();
		}
		++framesCounted;
	}

	// Downscale the current image into a temporary buffer
	downscale(frame, *downscaleBuff);

	// The first frame is copied to the reference image to avoid the formation of a screen-wide delta for one frame.
	if (firstFrame) {
		memcpy(currentImage->getPixels(), downscaleBuff->getPixels(), imageSize);
		memcpy(refImage->getPixels(), downscaleBuff->getPixels(), imageSize);
		firstFrame = false;
		// No motion on the first frame. Wipe the motion channel (0)
		uint8_t* mask = motionMask->getPixels();
		uint8_t* mEnd = mask + motionMask->getTotalSize();
		for (; mask < mEnd; mask += motionMask->getBytesPerPixel()) {
			mask[0] = 0;
		}
		return *motionMask;
	}

	// See if the current image has changed significantly
	unsigned int* currentTime = currentStableTimes.data();
	uint8_t* tip = downscaleBuff->getPixels();
	uint8_t* bmp = motionMask->getPixels();
	uint8_t* cip = currentImage->getPixels();
	uint8_t* currEnd = cip + imageSize;
	for (; cip < currEnd; cip += kBytesPerPixel, tip += kBytesPerPixel,
	        bmp += kBytesPerPixel, ++currentTime) {
		if (pixelIsDifferent(tip, cip)) {
			*currentTime = 0;
			memcpy(cip, tip, kBytesPerPixel);
		}
		// If the pixel has not changed significantly, nudge it towards its current value
		else {
			++(*currentTime);
			for (size_t b = 0; b < kBytesPerPixel; ++b) {
				cip[b] += Math::sign(tip[b] - cip[b]);
			}
		}
	}

	// If the current pixel has set a new stability record or is close to the
	// background pixel, copy it over. Also light up our blob map.
	currentTime = currentStableTimes.data();
	unsigned int* record = stableRecords;
	uint8_t* rip = refImage->getPixels();
	bmp = motionMask->getPixels();
	cip = currentImage->getPixels();
	for (; cip < currEnd; cip += kBytesPerPixel, rip += kBytesPerPixel,
	        bmp += kBytesPerPixel, ++currentTime, ++record) {
		if (*currentTime > *record) {
			memcpy(rip, cip, kBytesPerPixel);
			*record = min(*currentTime, stableCap);
		}
		// If the reference image pixel is significantly different from the current image pixel,
		// the pixel is considered to be moving
		/// \todo Optimize by removing branching?
		if (pixelIsDifferent(rip, cip))
			bmp[0] = 255;
		else
			bmp[0] = 0;
	}

	// Erosion pass
	if (erosionLevel > 0) {
		int x = 0;
		int y = 0;
		uint8_t* eroded = erodedMask->getPixels();
		uint8_t* bEnd = motionMask->getPixels() + motionMask->getTotalSize();
		for (bmp = motionMask->getPixels(); bmp < bEnd; bmp += kBytesPerPixel, eroded += kBytesPerPixel) {
			// If the current pixel is "moving"
			if (bmp[0] > 0) {
				// Find the number of adjacent moving pixels
				int adjacents = 0;
				for (auto it = offs.begin(); it != offs.end(); ++it) {
					if (x + it->x > 0 &&
						y + it->y > 0 &&
						(bmp + it->p)[0] > 0)
						++adjacents;
				}
				if (adjacents >= erosionLevel)
					eroded[0] = bmp[0];
				else
					eroded[0] = 0;
			}
			else {
				eroded[0] = 0;
			}
			eroded[1] = bmp[1];
			eroded[2] = bmp[2];

			if (++x == (int)motionMask->getWidth()) {
				++y;
				x = 0;
			}
		}
		*motionMask = *erodedMask;

		x = 0;
		y = 0;
		eroded = erodedMask->getPixels();
		for (bmp = motionMask->getPixels(); bmp < bEnd; bmp += kBytesPerPixel, eroded += kBytesPerPixel) {
			// Any pixel that is either on itself or has neighbors that are on is turned on
			if (bmp[0] > 0) {
				eroded[0] = bmp[0];
			}
			else {
				bool activeNeighbors = false;
				for (auto it = offs.begin(); it != offs.end(); ++it) {
					if (x + it->x > 0 &&
						y + it->y > 0 &&
						(bmp + it->p)[0] > 0) {
						activeNeighbors = true;
						break;
					}
				}
				eroded[0] = activeNeighbors ? 255 : 0;
			}
			eroded[1] = bmp[1];
			eroded[2] = bmp[2];

			if (++x == (int)motionMask->getWidth()) {
				++y;
				x = 0;
			}
		}
		*motionMask = *erodedMask;
	}
	return *motionMask;
}

void MotionExtractor::reset()
{
	// For comparison purposes, it is important that pixel timers start at zero
	fill(currentStableTimes.begin(), currentStableTimes.end(), 0);
	memset(stableRecords, 0, imageArea * sizeof(unsigned int));

	// The first frame will be used to wipe the reference and current images.
	firstFrame = true;
}

void MotionExtractor::downscale(const VideoFrame& frame, VideoFrame& down)
{
	const uint8_t* srcPixel = frame.getPixels();
	unsigned short* accum = downscaleAccumulators.data();
	unsigned short* accumRow = accum;
	unsigned short* accumEnd = accum + downscaleAccumulators.size();

	int accumRowsDone = 0;
	size_t rowPixelsDone = 0;

	// Clear the accumulators
	fill(downscaleAccumulators.begin(), downscaleAccumulators.end(), 0);

	// source rows accumulated for the current accumulator row
	int srcRowsPerAccumRow = 0;

	while (accum < accumEnd) {

		// Accumulate the downscale ratio of pixels, then advance the accumulator pointer
		for (size_t p = 0; p < kDownscaleRatio; ++p) {
			for (size_t c = 0; c < kBytesPerPixel; ++c)
				accum[c] += srcPixel[c];

			srcPixel += kBytesPerPixel;
		}
		accum += kBytesPerPixel;

		// If we've reached the end of a accumulator row
		if (++rowPixelsDone == imageWidth) {
			rowPixelsDone = 0;

			// Only move to a new accumulator row if we've accumulated the correct number of source rows
			if (++srcRowsPerAccumRow == kDownscaleRatio) {
				++accumRowsDone;
				accumRow = downscaleAccumulators.data() + accumRowsDone * destLineSize;
				srcRowsPerAccumRow = 0;
			}

			// Move back to the start of the row, or advance to the next one
			accum = accumRow;
			srcPixel = frame.getPixels() + (accumRowsDone * kDownscaleRatio + srcRowsPerAccumRow) * srcLineSize;
		}
	}
	// Now that we've accumulated everything, just divide it by the number of pixels accumulated
	auto dp = down.getPixels();
	for (accum = downscaleAccumulators.data(); accum < accumEnd; ++accum, ++dp)
		*dp = *accum / kDownscaleSquare;
}

void MotionExtractor::setSensitivity(int newSens)
{
	if (newSens < 1 || newSens > 127)
		throw Exceptions::ArgumentOutOfRangeException("Sensitivity must be between 1 and 127", __FUNCTION__);

	motionThreshold = newSens;
	reset();
}

void MotionExtractor::setSettleTime(double newTime)
{
	// Values outside this range don't work very well
	if (newTime < 1 || newTime > 60)
		throw Exceptions::ArgumentOutOfRangeException("Settle time must be between 1 and 60 seconds", __FUNCTION__);

	stableCap = (unsigned int)ceil(newTime * fps);
	reset();
}

void MotionExtractor::setErosion(int newErosion)
{
	if (newErosion < 0 || newErosion > 8)
		throw Exceptions::ArgumentOutOfRangeException("Erosion value must be between 0 and 8 pixels", __FUNCTION__);

	erosionLevel = newErosion;
	reset();
}

int MotionExtractor::getSensitivity() const
{
	return motionThreshold;
}

double MotionExtractor::getSettleTime() const
{
	return (double)stableCap / fps;
}

int MotionExtractor::getErosion() const
{
	return erosionLevel;
}

void MotionExtractor::save(Json::Value& paramsObject) const
{
	paramsObject["sensitivity"] = getSensitivity();
	paramsObject["settle time"] = getSettleTime();
	paramsObject["erosion level"] = getErosion();
}

void MotionExtractor::load(Json::Value& paramsObject)
{
	const Json::Value& sv = paramsObject["sensitivity"];
	const Json::Value& tv = paramsObject["settle time"];
	const Json::Value& ev = paramsObject["erosion level"];
	if (sv.isNull() || tv.isNull() || ev.isNull())
		throw Exceptions::FileException("Motion detection settings are missing", __FUNCTION__);

	const int si = sv.asInt();
	const double td = tv.asDouble();
	const int ei = ev.asInt();

	if (si < 1 || si > 127 || td < 1 || td > 60 || ei < 0 || ei > 8)
		throw Exceptions::FileException("Motion detection settings are invalid", __FUNCTION__);

	setSensitivity(si);
	setSettleTime(td);
	setErosion(ei);
}
