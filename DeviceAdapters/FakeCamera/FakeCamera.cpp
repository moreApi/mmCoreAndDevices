///////////////////////////////////////////////////////////////////////////////
// FILE:          FakeCamera.cpp
// PROJECT:       Micro-Manager
// SUBSYSTEM:     DeviceAdapters
//-----------------------------------------------------------------------------
// DESCRIPTION:   A camera implementation that is backed by the file system
//                Can access stage positions to choose image to display
//
// AUTHOR:        Lukas Lang
//
// COPYRIGHT:     2017 Lukas Lang
// LICENSE:       Licensed under the Apache License, Version 2.0 (the "License");
//                you may not use this file except in compliance with the License.
//                You may obtain a copy of the License at
//                
//                http://www.apache.org/licenses/LICENSE-2.0
//                
//                Unless required by applicable law or agreed to in writing, software
//                distributed under the License is distributed on an "AS IS" BASIS,
//                WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//                See the License for the specific language governing permissions and
//                limitations under the License.

#include "FakeCamera.h"

const char* cameraName = "FakeCamera";

const char* label_CV_8U = "8bit";
const char* label_CV_16U = "16bit";
const char* label_CV_8UC4 = "32bitRGB";
const char* label_CV_16UC4 = "64bitRGB";

FakeCamera::FakeCamera() :
	initialized_(false),
	path_(""),
	capturing_(false),
	color_(false),
	roiX_(0),
	roiY_(0),
	cameraWidth_(64),
	cameraHeight_(64),
	posX_(0),
	posY_(0),
	byteCount_(1),
	type_(CV_8UC1),
	emptyImg(1, 1, type_),
	exposure_(10),
	useTiffStack_(false),
	timePoints_(0),
	updateRateMil_(0)
{
	resetCurImg();

	CreateProperty("Path mask", "", MM::String, false, new CPropertyAction(this, &FakeCamera::OnPath));
	CreateProperty("Resolved path", "", MM::String, true, new CPropertyAction(this, &FakeCamera::ResolvePath));

	CreateProperty("FrameCount", "0", MM::Integer, false, new CPropertyAction(this, &FakeCamera::OnFrameCount));

	std::vector<std::string> allowedValues;
	allowedValues.push_back("0");
	allowedValues.push_back("1");
	CreateProperty("Tiff Stack", "0", MM::Integer, false, new CPropertyAction(this, &FakeCamera::OnTiffStack));
	SetAllowedValues("Tiff Stack", allowedValues);

	//allowedValues.clear();
	CreateProperty("Time Points", "0", MM::Integer, false, new CPropertyAction(this, &FakeCamera::OnTimePoints));
	CreateProperty("Update Rate Milliseconds", "1000", MM::Integer, false, new CPropertyAction(this, &FakeCamera::OnUpdateRateMil));

	CreateProperty("Camera Width", "64", MM::Integer, false, new CPropertyAction(this, &FakeCamera::OnUpdateCamWidth));
	CreateProperty("Camera Height", "64", MM::Integer, false, new CPropertyAction(this, &FakeCamera::OnUpdateCamHeight));


	CreateProperty(MM::g_Keyword_Name, cameraName, MM::String, true);

	// Description
	CreateProperty(MM::g_Keyword_Description, "Loads images from disk according to position of focusing stage", MM::String, true);

	// CameraName
	CreateProperty(MM::g_Keyword_CameraName, "Fake camera adapter", MM::String, true);

	// CameraID
	CreateProperty(MM::g_Keyword_CameraID, "V1.1", MM::String, true);

	// binning
	CreateProperty(MM::g_Keyword_Binning, "1", MM::Integer, false);

	std::vector<std::string> binningValues;
	binningValues.push_back("1");

	SetAllowedValues(MM::g_Keyword_Binning, binningValues);

	CreateStringProperty(MM::g_Keyword_PixelType, label_CV_8U, false, new CPropertyAction(this, &FakeCamera::OnPixelType));

	std::vector<std::string> pixelTypeValues;
	pixelTypeValues.push_back(label_CV_8U);
	pixelTypeValues.push_back(label_CV_16U);
	pixelTypeValues.push_back(label_CV_8UC4);
	pixelTypeValues.push_back(label_CV_16UC4);

	SetAllowedValues(MM::g_Keyword_PixelType, pixelTypeValues);

	SetErrorText(ERR_INVALID_DEVICE_NAME, "Specified stage name is invalid");
	SetErrorText(OUT_OF_RANGE, "Parameters out of range");

	InitializeDefaultErrorMessages();
}

FakeCamera::~FakeCamera()
{
}

int FakeCamera::Initialize()
{
	if (initialized_)
		return DEVICE_OK;

	initSize_ = false;

	initialized_ = true;

	return DEVICE_OK;
}

int FakeCamera::Shutdown()
{
	initialized_ = false;

	return DEVICE_OK;
}

void FakeCamera::GetName(char* name) const
{
	CDeviceUtils::CopyLimitedString(name, cameraName);
}

long FakeCamera::GetImageBufferSize() const
{
	initSize();

	return roiWidth_ * roiHeight_ * GetImageBytesPerPixel();
}

unsigned FakeCamera::GetBitDepth() const
{
	initSize();

	return 8 * byteCount_;
}

int FakeCamera::GetBinning() const
{
	return 1;
}

int FakeCamera::SetBinning(int)
{
	return DEVICE_OK;
}

void FakeCamera::SetExposure(double exposure)
{
	exposure_ = exposure;
}

double FakeCamera::GetExposure() const
{
	return exposure_;
}

int FakeCamera::SetROI(unsigned x, unsigned y, unsigned xSize, unsigned ySize)
{
	initSize();

	if (x + xSize > width_ || y + ySize > height_)
	{
		return OUT_OF_RANGE;
	}

	roiX_ = x;
	roiY_ = y;
	roiWidth_ = xSize;
	roiHeight_ = ySize;

	updateROI();

	return DEVICE_OK;
}

int FakeCamera::GetROI(unsigned& x, unsigned& y, unsigned& xSize, unsigned& ySize)
{
	initSize();

	x = roiX_;
	y = roiY_;
	xSize = roiWidth_;
	ySize = roiHeight_;

	return DEVICE_OK;
}

int FakeCamera::ClearROI()
{
	initSize();

	SetROI(posX_, posY_, width_, height_);

	return DEVICE_OK;
}

int FakeCamera::IsExposureSequenceable(bool& isSequenceable) const
{
	isSequenceable = false;

	return DEVICE_OK;
}

const unsigned char* FakeCamera::GetImageBuffer()
{
	return roi_.data;
}

unsigned FakeCamera::GetNumberOfComponents() const
{
	return color_ ? 4 : 1;
}

const unsigned int* FakeCamera::GetImageBufferAsRGB32()
{
	return color_ ? (const unsigned int*)roi_.data : 0;
}

unsigned FakeCamera::GetImageWidth() const
{
	initSize();

	return roiWidth_;
}

unsigned FakeCamera::GetImageHeight() const
{
	initSize();

	return roiHeight_;
}

unsigned FakeCamera::GetImageBytesPerPixel() const
{
	return color_ ? 4 * byteCount_ : byteCount_;
}

int FakeCamera::SnapImage()
{
	ERRH_START
		MM::MMTime start = GetCoreCallback()->GetCurrentMMTime();
	++frameCount_;
	initSize();

	getImg();

	MM::MMTime end = GetCoreCallback()->GetCurrentMMTime();

	double rem = exposure_ - (end - start).getMsec();

	if (rem > 0)
		CDeviceUtils::SleepMs((long)rem);
	ERRH_END
}

int FakeCamera::StartSequenceAcquisition(long numImages, double interval_ms, bool stopOnOverflow)
{
	capturing_ = true;
	return CCameraBase::StartSequenceAcquisition(numImages, interval_ms, stopOnOverflow);
}

int FakeCamera::StopSequenceAcquisition()
{
	capturing_ = false;
	return CCameraBase::StopSequenceAcquisition();
}

void FakeCamera::OnThreadExiting() throw()
{
	capturing_ = false;
	CCameraBase::OnThreadExiting();
}

int FakeCamera::OnPath(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(path_.c_str());
	}
	else if (eAct == MM::AfterSet)
	{
		std::string oldPath = path_;
		pProp->Get(path_);
		resetCurImg();

		if (initialized_)
		{
			ERRH_START
				try
			{
				getImg();
			}
			catch (error_code ex)
			{
				pProp->Set(oldPath.c_str());
				path_ = oldPath;
				throw ex;
			}
			ERRH_END
		}
	}

	return DEVICE_OK;
}

int FakeCamera::ResolvePath(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		try
		{
			pProp->Set(parseMask(path_).c_str());
		}
		catch (error_code)
		{
			pProp->Set("[Invalid path specification]");
		}
	}

	return DEVICE_OK;
}

double scaleFac(int bef, int aft)
{
	return (double)(1 << (8 * aft)) / (1 << (8 * bef));
}

int FakeCamera::OnPixelType(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		switch (type_)
		{
		case CV_8UC1:
			pProp->Set(label_CV_8U);
			break;
		case CV_16UC1:
			pProp->Set(label_CV_16U);
			break;
		case CV_8UC4:
			pProp->Set(label_CV_8UC4);
			break;
		case CV_16UC4:
			pProp->Set(label_CV_16UC4);
			break;

		}
	}
	else if (eAct == MM::AfterSet)
	{
		if (capturing_)
			return DEVICE_CAMERA_BUSY_ACQUIRING;

		std::string val;
		pProp->Get(val);

		if (val == label_CV_16U)
		{
			byteCount_ = 2;
			color_ = false;
			type_ = CV_16UC1;
		}
		else if (val == label_CV_8UC4)
		{
			byteCount_ = 1;
			color_ = true;
			type_ = CV_8UC4;
		}
		else if (val == label_CV_16UC4)
		{
			byteCount_ = 2;
			color_ = true;
			type_ = CV_16UC4;
		}
		else
		{
			byteCount_ = 1;
			color_ = false;
			type_ = CV_8UC1;
		}

		emptyImg = cv::Mat::zeros(1, 1, type_);
		// emptyImg = 0;

		resetCurImg();
	}

	return DEVICE_OK;
}

int FakeCamera::OnFrameCount(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(CDeviceUtils::ConvertToString(frameCount_));
	}
	else if (eAct == MM::AfterSet)
	{
		std::string val;
		pProp->Get(val);
		resetCurImg();
		frameCount_ = atoi(val.c_str());
	}

	return DEVICE_OK;
}

int FakeCamera::OnTiffStack(MM::PropertyBase* pProp, MM::ActionType eAct) {

	if (eAct == MM::BeforeGet)
	{
		pProp->Set(CDeviceUtils::ConvertToString((int)useTiffStack_));
	}
	else if (eAct == MM::AfterSet)
	{
		std::string val;
		pProp->Get(val);
		resetCurImg();
		useTiffStack_ = atoi(val.c_str()) == 1;
	}

	return DEVICE_OK;
}

int FakeCamera::OnTimePoints(MM::PropertyBase* pProp, MM::ActionType eAct) {

	if (eAct == MM::BeforeGet)
	{
		pProp->Set(CDeviceUtils::ConvertToString(timePoints_));
	}
	else if (eAct == MM::AfterSet)
	{
		std::string val;
		pProp->Get(val);
		resetCurImg();
		timePoints_ = atoi(val.c_str());
	}

	return DEVICE_OK;
}

int FakeCamera::OnUpdateRateMil(MM::PropertyBase* pProp, MM::ActionType eAct) {

	if (eAct == MM::BeforeGet)
	{
		pProp->Set(CDeviceUtils::ConvertToString(updateRateMil_));
	}
	else if (eAct == MM::AfterSet)
	{
		std::string val;
		pProp->Get(val);
		resetCurImg();
		updateRateMil_ = atoi(val.c_str());
	}

	return DEVICE_OK;
}

int FakeCamera::OnUpdateCamWidth(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(CDeviceUtils::ConvertToString(cameraWidth_));
	}
	else if (eAct == MM::AfterSet)
	{
		std::string val;
		pProp->Get(val);
		resetCurImg();
		cameraWidth_ = atoi(val.c_str());
	}

	return DEVICE_OK;
}

int FakeCamera::OnUpdateCamHeight(MM::PropertyBase* pProp, MM::ActionType eAct)
{
	if (eAct == MM::BeforeGet)
	{
		pProp->Set(CDeviceUtils::ConvertToString(cameraHeight_));
	}
	else if (eAct == MM::AfterSet)
	{
		std::string val;
		pProp->Get(val);
		resetCurImg();
		cameraHeight_ = atoi(val.c_str());
	}

	return DEVICE_OK;
}

/* parse and replace
goes throu the string and calls parsePlaceholde when it hits a '?'
*/
std::string FakeCamera::parseUntil(const char*& it, const char delim) const throw (parse_error)
{
	std::ostringstream ret;

	for (; *it != '\0' && *it != delim; ++it)
	{
		if (*it == '?')
			ret << parsePlaceholder(it);
		else
			ret << *it;
	}

	if (*it != delim)
		throw parse_error();

	return ret.str();
}

/* parse and replace
reads the next char after an inital '?'
*/
std::string FakeCamera::parsePlaceholder(const char*& it) const
{
	const char* start = it;
	++it;

	try
	{
		std::pair<int, int> precSpec(1, 0);
		std::string metadata("");
		std::string name("");


		for (; *it != 0; ++it)
		{
			switch (*it)
			{
			case '{':
				precSpec = parsePrecision(++it);
				break;
			case '(':
				metadata = parseUntil(++it, ')');
				break;
			case '[':
				name = parseUntil(++it, ']');
				break;
			case '?':
				name = "?";
				break;
			case '!':
				//increase amount of leading zeros
				precSpec.first++;
				break;
			default:
				throw parse_error();
			}

			if (name.size() > 0)
				break;
		}

		if (name.size() == 0)
			throw parse_error();

		std::ostringstream res;

		if (name == "?")
		{
			double val;
			if (timePoints_ == 0) {
				if (GetCoreCallback()->GetFocusPosition(val) != 0)
					val = 0;
			}
			else {
				std::time_t now = std::time(0);
				val = now % timePoints_;
			}

			printNum(res, precSpec, val);
			return res.str();
		}

		if (name == "$frame")
		{
			int val = frameCount_;

			if (metadata.size() > 0)
			{
				int max = atoi(metadata.c_str());
				if (max == 0)
					max = 1;

				val %= max;
			}

			printNum(res, precSpec, val);
			return res.str();
		}

		MM::Device* dev = GetCoreCallback()->GetDevice(this, name.c_str());

		if (dev == 0)
			throw parse_error();

		switch (dev->GetType())
		{
		case MM::ShutterDevice:
		{
			bool open;
			if (((MM::Shutter*)dev)->GetOpen(open) != 0)
				open = false;

			if (metadata.size() == 0)
				printNum(res, precSpec, open ? 1 : 0);
			else
				res << iif(open, metadata);
		}
		break;
		case MM::StateDevice:
		{
			MM::State* state = (MM::State*)dev;

			if (metadata == "$name")
			{
				char label[MM::MaxStrLength];
				if (state->GetPosition(label) != 0)
					label[0] = '\0';
				res << label;
			}
			else if (metadata.size() > 0)
			{
				bool open;
				if (state->GetGateOpen(open) != 0)
					open = false;

				res << iif(open, metadata);
			}
			else
			{
				long pos;
				if (state->GetPosition(pos))
					pos = 0;

				printNum(res, precSpec, pos);
			}
		}
		break;
		case MM::XYStageDevice:
		case MM::GalvoDevice:
		{
			double x, y;

			if (dev->GetType() == MM::XYStageDevice ? ((MM::XYStage*)dev)->GetPositionUm(x, y) : ((MM::Galvo*)dev)->GetPosition(x, y))
				x = y = 0;

			if (metadata == "$x")
				printNum(res, precSpec, x);
			else if (metadata == "$y")
				printNum(res, precSpec, y);
			else
			{
				std::string sep = metadata.size() > 0 ? metadata : "-";
				printNum(printNum(res, precSpec, x) << sep, precSpec, y);
			}
		}
		break;
		case MM::StageDevice:
		{
			double pos;
			if (((MM::Stage*)dev)->GetPositionUm(pos) != 0)
				pos = 0;

			printNum(res, precSpec, pos);
		}
		break;
		case MM::SignalIODevice:
		{
			MM::SignalIO* signalIO = (MM::SignalIO*)dev;

			if (metadata.size() > 0)
			{
				bool open;
				if (signalIO->GetGateOpen(open) != 0)
					open = false;

				res << iif(open, metadata);
			}
			else
			{
				double vol;
				if (signalIO->GetSignal(vol) != 0)
					vol = 0;

				printNum(res, precSpec, vol);
			}
		}
		break;
		case MM::MagnifierDevice:
			printNum(res, precSpec, ((MM::Magnifier*)dev)->GetMagnification());
			break;

		default:
			throw parse_error();
		}

		return res.str();
	}
	catch (parse_error)
	{
		it = start;
		return std::string(start, 1);
	}
}

std::pair<int, int> FakeCamera::parsePrecision(const char*& it) const throw (parse_error)
{
	std::string pSpec = parseUntil(it, '}');

	size_t dotPos = pSpec.find_first_of('.');

	if (dotPos > 0)
		return std::pair<int, int>(atoi(pSpec.substr(0, dotPos).c_str()), atoi(pSpec.substr(dotPos + 1).c_str()));

	return std::pair<int, int>(0, atoi(pSpec.c_str()));
}

std::ostream& FakeCamera::printNum(std::ostream& o, std::pair<int, int> precSpec, double num)
{
	int intLen = precSpec.first;
	int prec = precSpec.second;

	//force -0.0 to be interpreted as 0.0
	if (num == 0)
		num = 0;

	if (num < 0)
	{
		o << '-';
		num = -num;
	}

	//set decimal places
	o << std::fixed << std::setprecision(prec);

	//set leading zeros by setting total length of number
	o << std::setfill('0') << std::setw(intLen + (prec == 0 ? 0 : prec + 1));

	o << num;
	return o;
}

//if spec contains a ':', this returns the part before if test is false, and the part after otherwise
//else, an empty string is returned if test is false, and spec otherwise
std::string FakeCamera::iif(bool test, std::string spec)
{
	size_t sepPos = spec.find_first_of(':');
	return test ? spec.substr(sepPos + 1) : spec.substr(0, sepPos + 1);
}

std::string FakeCamera::parseMask(std::string mask) const throw(error_code)
{
	const char* it = mask.data();
	return parseUntil(it, '\0');
}

void FakeCamera::getImg() const
{
	cv::Mat img;
	std::string path = parseMask(path_);

	if (useTiffStack_) {
		std::vector<cv::Mat> currentStack;

		if (timePoints_ == 0) {
			if (tiffStacks_.empty()) {
				std::vector<cv::Mat> stack;
				if (!cv::imreadmulti(path, stack, cv::IMREAD_ANYDEPTH | (color_ ? cv::IMREAD_COLOR : cv::IMREAD_GRAYSCALE))) {
					throw error_code(CONTROLLER_ERROR, "Could not find image '" + path + "'. Please specify a vaild path to a tiff stack.");
				}
				tiffStacks_["noTime"] = stack;
			}
			currentStack = tiffStacks_["noTime"];
		}
		else {
			auto search = tiffStacks_.find(path);
			if (search != tiffStacks_.end()) {
				currentStack = search->second;
			}
			else {
				std::vector<cv::Mat> stack;
				if (!cv::imreadmulti(path, stack, cv::IMREAD_ANYDEPTH | (color_ ? cv::IMREAD_COLOR : cv::IMREAD_GRAYSCALE))) {
					throw error_code(CONTROLLER_ERROR, "Could not find image '" + path + "'. Please specify a vaild path to a tiff stack.");
				}
				tiffStacks_[path] = stack;
				currentStack = stack;
			}
			
		}


		double focusDepth;
		if (GetCoreCallback()->GetFocusPosition(focusDepth) != 0)
			focusDepth = 0;

		int index = std::max(std::min((int)focusDepth,(int)currentStack.size()-1),0);
		img = currentStack[index];
	}
	else {

		if (path == curPath_)
			return;

		img = path == lastFailedPath_ ? lastFailedImg_ : cv::imread(path, cv::IMREAD_ANYDEPTH | (color_ ? cv::IMREAD_COLOR : cv::IMREAD_GRAYSCALE));

		if (img.data == NULL)
		{
			if (curImg_.data != NULL)
			{
				LogMessage("Could not find image '" + path + "', reusing last valid image");
				curPath_ = path;
				return;
			}
			else
			{
				throw error_code(CONTROLLER_ERROR, "Could not find image '" + path + "'. Please specify a valid path mask (format: ?? for focus stage, ?[name] for any stage, and ?{prec}[name]/?{prec}? for precision other than 0)");
			}
		}
	}

	img.convertTo(img, type_, scaleFac((int)img.elemSize() / img.channels(), byteCount_));

	bool dimChanged = (unsigned)img.cols != width_ || (unsigned)img.rows != height_;

	if (dimChanged)
	{
		if (capturing_)
		{
			lastFailedPath_ = path;
			lastFailedImg_ = img;
			throw error_code(DEVICE_CAMERA_BUSY_ACQUIRING);
		}
	}

	if (color_)
	{
		if (alphaChannel_.rows != img.rows || alphaChannel_.cols != img.cols || alphaChannel_.depth() != img.depth())
		{
			alphaChannel_ = cv::Mat(img.rows, img.cols, byteCount_ == 2 ? CV_16U : CV_8U);
			alphaChannel_ = 1 << (8 * byteCount_);
		}

		if (dimChanged)
			curImg_ = cv::Mat(img.rows, img.cols, type_);

		int fromTo[] = { 0,0 , 1,1 , 2,2 , 3,3 };
		cv::Mat from[] = { img, alphaChannel_ };

		cv::mixChannels(from, 2, &curImg_, 1, fromTo, 4);
	}
	else
		curImg_ = img;

	curPath_ = path;

	if (dimChanged)
	{
		initSize_ = false;
		initSize(false);
	}
	
	//should create an zero initialized image of size camera
	cv::Mat imageView = cv::Mat::zeros(cameraHeight_, cameraWidth_, type_);

	double posX, posY;
	GetCoreCallback()->GetXYPosition(posX, posY);
	double posXLeft = posX < 0.0 ? 1.0 : 0.0;
	double posYUp = posY < 0.0 ? 1.0 : 0.0;

	posX = posX < -cameraWidth_ ? -cameraWidth_ : posX;
	posX = posX > img.cols ? img.cols : posX;
	posY = posY < -cameraHeight_ ? -cameraHeight_ : posY;
	posY = posY > img.rows ? img.rows : posY;
	GetCoreCallback()->SetXYPosition(posX, posY);

	double viewFromWholeWidth = std::max(1.0, std::min(posX + cameraWidth_, (double)img.cols) - std::max(0.0, std::min(posX, (double)img.cols - 1)));
	double viewFromWholeHeight = std::max(1.0, std::min(posY + cameraHeight_, (double)img.rows) - std::max(0.0, std::min(posY, (double)img.rows - 1)));


	cv::Mat cameraViewFromWhole = curImg_(cv::Range(std::max(0.0, std::min(posY, (double)img.rows - 1)), std::max(1.0, std::min(posY + cameraHeight_, (double)img.rows)))
										, cv::Range(std::max(0.0, std::min(posX, (double)img.cols - 1)), std::max(1.0, std::min(posX + cameraWidth_, (double)img.cols))) );

	cv::Rect copyDest(cv::Point( (int)(cameraWidth_ - (posXLeft * (viewFromWholeWidth))) % (cameraWidth_), (int)(cameraHeight_ - (posYUp * (viewFromWholeHeight))) % (cameraHeight_) ), cameraViewFromWhole.size());

	cameraViewFromWhole.copyTo(imageView(copyDest));


	roi_ = imageView;
	/*roiX_ = std::max(0.0, std::min(posX + (img.cols / 2), (double)img.cols)-cameraWidth_);
	roiY_ = std::max(0.0, std::min(posY + (img.rows / 2), (double)img.rows)-cameraHeight_);


	roiHeight_ = std::max(1, std::min(cameraHeight_, img.rows));
	roiWidth_ = std::max(1, std::min(cameraWidth_, img.cols));*/
	updateROI();
}

void FakeCamera::updateROI() const
{
	//roi_ = curImg_(cv::Range(roiY_, roiY_ + roiHeight_), cv::Range(roiX_, roiX_ + roiWidth_));

	if (!roi_.isContinuous())
		roi_ = roi_.clone();
}

void FakeCamera::initSize(bool loadImg) const
{
	if (initSize_)
		return;

	initSize_ = true;

	try
	{
		if (loadImg)
			getImg();

		roiWidth_ = width_ = curImg_.cols;
		roiHeight_ = height_ = curImg_.rows;
	}
	catch (error_code)
	{
		roiWidth_ = width_ = 1;
		roiHeight_ = height_ = 1;

		initSize_ = false;
	}
}

void FakeCamera::resetCurImg()
{
	initSize_ = false;
	curPath_ = "";
	curImg_ = emptyImg;
	roiWidth_ = width_ = 1;
	roiHeight_ = height_ = 1;
	frameCount_ = 0;

	tiffStacks_.clear();

	ClearROI();
	updateROI();
}
