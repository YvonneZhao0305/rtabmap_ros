/*
Copyright (c) 2010-2014, cv::Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other cv::Materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "rtabmap/core/util2d.h"

#include <rtabmap/utilite/ULogger.h>
#include <rtabmap/utilite/UMath.h>
#include <rtabmap/utilite/UConversion.h>
#include <rtabmap/utilite/UTimer.h>
#include <rtabmap/utilite/UStl.h>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <map>

namespace rtabmap
{

namespace util2d
{

// SSD: Sum of Squared Differences
float ssd(const cv::Mat & windowLeft, const cv::Mat & windowRight)
{
	UASSERT_MSG(windowLeft.type() == CV_8UC1 || windowLeft.type() == CV_32FC1 || windowLeft.type() == CV_16SC2, uFormat("Type=%d", windowLeft.type()).c_str());
	UASSERT(windowLeft.type() == windowRight.type());
	UASSERT_MSG(windowLeft.rows == windowRight.rows, uFormat("%d vs %d", windowLeft.rows, windowRight.rows).c_str());
	UASSERT_MSG(windowLeft.cols == windowRight.cols, uFormat("%d vs %d", windowLeft.cols, windowRight.cols).c_str());

	float score = 0.0f;
	for(int v=0; v<windowLeft.rows; ++v)
	{
		for(int u=0; u<windowLeft.cols; ++u)
		{
			float s = 0.0f;
			if(windowLeft.type() == CV_8UC1)
			{
				s = float(windowLeft.at<unsigned char>(v,u))-float(windowRight.at<unsigned char>(v,u));
			}
			else if(windowLeft.type() == CV_32FC1)
			{
				s = windowLeft.at<float>(v,u)-windowRight.at<float>(v,u);
			}
			else if(windowLeft.type() == CV_16SC2)
			{
				float sL = float(windowLeft.at<cv::Vec2s>(v,u)[0])*0.5f+float(windowLeft.at<cv::Vec2s>(v,u)[1])*0.5f;
				float sR = float(windowRight.at<cv::Vec2s>(v,u)[0])*0.5f+float(windowRight.at<cv::Vec2s>(v,u)[1])*0.5f;
				s = sL - sR;
			}

			score += s*s;
		}
	}
	return score;
}

// SAD: Sum of Absolute intensity Differences
float sad(const cv::Mat & windowLeft, const cv::Mat & windowRight)
{
	UASSERT_MSG(windowLeft.type() == CV_8UC1 || windowLeft.type() == CV_32FC1 || windowLeft.type() == CV_16SC2, uFormat("Type=%d", windowLeft.type()).c_str());
	UASSERT(windowLeft.type() == windowRight.type());
	UASSERT_MSG(windowLeft.rows == windowRight.rows, uFormat("%d vs %d", windowLeft.rows, windowRight.rows).c_str());
	UASSERT_MSG(windowLeft.cols == windowRight.cols, uFormat("%d vs %d", windowLeft.cols, windowRight.cols).c_str());

	float score = 0.0f;
	for(int v=0; v<windowLeft.rows; ++v)
	{
		for(int u=0; u<windowLeft.cols; ++u)
		{
			if(windowLeft.type() == CV_8UC1)
			{
				score += fabs(float(windowLeft.at<unsigned char>(v,u))-float(windowRight.at<unsigned char>(v,u)));
			}
			else if(windowLeft.type() == CV_32FC1)
			{
				score += fabs(windowLeft.at<float>(v,u)-windowRight.at<float>(v,u));
			}
			else if(windowLeft.type() == CV_16SC2)
			{
				float sL = float(windowLeft.at<cv::Vec2s>(v,u)[0])*0.5f+float(windowLeft.at<cv::Vec2s>(v,u)[1])*0.5f;
				float sR = float(windowRight.at<cv::Vec2s>(v,u)[0])*0.5f+float(windowRight.at<cv::Vec2s>(v,u)[1])*0.5f;
				score += fabs(sL - sR);
			}
		}
	}
	return score;
}

std::vector<cv::Point2f> calcStereoCorrespondences(
		const cv::Mat & leftImage,
		const cv::Mat & rightImage,
		const std::vector<cv::Point2f> & leftCorners,
		std::vector<unsigned char> & status,
		cv::Size winSize,
		int maxLevel,
		int iterations,
		int minDisparity,
		int maxDisparity,
		bool ssdApproach)
{
	UDEBUG("winSize=(%d,%d)", winSize.width, winSize.height);
	UDEBUG("maxLevel=%d", maxLevel);
	UDEBUG("minDisparity=%d", minDisparity);
	UDEBUG("maxDisparity=%d", maxDisparity);
	UDEBUG("iterations=%d", iterations);
	UDEBUG("ssdApproach=%d", ssdApproach?1:0);

	// window should be odd
	if(winSize.width%2 == 0)
	{
		winSize.width+=1;
	}
	if(winSize.height%2 == 0)
	{
		winSize.height+=1;
	}

	cv::Size halfWin((winSize.width-1)/2, (winSize.height-1)/2);

	UTimer timer;
	double pyramidTime = 0.0;
	double disparityTime = 0.0;
	double subpixelTime = 0.0;

	std::vector<cv::Point2f> rightCorners(leftCorners.size());
	std::vector<cv::Mat> leftPyramid, rightPyramid;
	maxLevel =  cv::buildOpticalFlowPyramid( leftImage, leftPyramid, winSize, maxLevel, false);
	maxLevel =  cv::buildOpticalFlowPyramid( rightImage, rightPyramid, winSize, maxLevel, false);
	pyramidTime = timer.ticks();

	status = std::vector<unsigned char>(leftCorners.size(), 0);
	int totalIterations = 0;
	int noSubPixel = 0;
	int added = 0;
	for(unsigned int i=0; i<leftCorners.size(); ++i)
	{
		int oi=0;
		float bestScore = -1.0f;
		float secondBest = -1.0f;
		int bestScoreIndex = -1;
		int tmpMinDisparity = minDisparity;
		int tmpMaxDisparity = maxDisparity;

		int iterations = 0;
		for(int level=maxLevel; level>=0; --level)
		{
			UASSERT(level < (int)leftPyramid.size());

			cv::Point2i center(int(leftCorners[i].x/float(1<<level)), int(leftCorners[i].y/float(1<<level)));

			oi=0;
			bestScore = -1.0f;
			secondBest = -1.0f;
			bestScoreIndex = -1;
			int localMaxDisparity = -tmpMaxDisparity / (1<<level);
			int localMinDisparity = -tmpMinDisparity / (1<<level);

			if(center.x-halfWin.width-(level==0?1:0) >=0 && center.x+halfWin.width+(level==0?1:0) < leftPyramid[level].cols &&
			   center.y-halfWin.height >=0 && center.y+halfWin.height < leftPyramid[level].rows)
			{
				cv::Mat windowLeft(leftPyramid[level],
						cv::Range(center.y-halfWin.height,center.y+halfWin.height+1),
						cv::Range(center.x-halfWin.width,center.x+halfWin.width+1));
				int minCol = center.x+localMaxDisparity-halfWin.width-1;
				if(minCol < 0)
				{
					localMaxDisparity -= minCol;
				}

				int maxCol = center.x+localMinDisparity+halfWin.width+1;
				if(maxCol >= leftPyramid[level].cols)
				{
					localMinDisparity += maxCol-leftPyramid[level].cols-1;
				}

				if(localMinDisparity < localMaxDisparity)
				{
					localMaxDisparity = localMinDisparity;
				}
				int length = localMinDisparity-localMaxDisparity+1;
				std::vector<float> scores = std::vector<float>(length, 0.0f);

				for(int d=localMinDisparity; d>localMaxDisparity; --d)
				{
					++iterations;
					cv::Mat windowRight(rightPyramid[level],
									cv::Range(center.y-halfWin.height,center.y+halfWin.height+1),
									cv::Range(center.x+d-halfWin.width,center.x+d+halfWin.width+1));
					scores[oi] = ssdApproach?ssd(windowLeft, windowRight):sad(windowLeft, windowRight);
					if(scores[oi] > 0 && (bestScore < 0.0f || scores[oi] < bestScore))
					{
						secondBest = bestScore;
						bestScoreIndex = oi;
						bestScore = scores[oi];
					}
					++oi;
				}

				if(bestScoreIndex>=0)
				{
					if(level>0)
					{
						UDEBUG("");
						tmpMaxDisparity = tmpMinDisparity+(bestScoreIndex+1)*(1<<level);
						tmpMaxDisparity+=tmpMaxDisparity%level;
						if(tmpMaxDisparity > maxDisparity)
						{
							tmpMaxDisparity = maxDisparity;
						}
						tmpMinDisparity = tmpMinDisparity+(bestScoreIndex-1)*(1<<level);
						tmpMinDisparity -= tmpMinDisparity%level;
						if(tmpMinDisparity < minDisparity)
						{
							tmpMinDisparity = minDisparity;
						}
					}
				}
			}
		}
		disparityTime+=timer.ticks();
		totalIterations+=iterations;

		if(bestScoreIndex>=0)
		{
			UDEBUG("");
			//subpixel refining
			int d = -(tmpMinDisparity+bestScoreIndex);

			cv::Mat windowLeft(winSize, CV_32FC1);
			cv::Mat windowRight(winSize, CV_32FC1);
			cv::getRectSubPix(leftPyramid[0],
					winSize,
					leftCorners[i],
					windowLeft,
					windowLeft.type());
			if(leftCorners[i].x != float(int(leftCorners[i].x)))
			{
				//recompute bestScore if the pt is not integer
				cv::getRectSubPix(rightPyramid[0],
						winSize,
						cv::Point2f(leftCorners[i].x+float(d), leftCorners[i].y),
						windowRight,
						windowRight.type());
				bestScore = ssdApproach?ssd(windowLeft, windowRight):sad(windowLeft, windowRight);
			}

			float xc = leftCorners[i].x+float(d);
			float vc = bestScore;
			float step = 0.5f;
			std::map<float, float> cache;
			bool reject = false;
			for(int it=0; it<iterations; ++it)
			{
				float x1 = xc-step;
				float x2 = xc+step;
				float v1 = uValue(cache, x1, 0.0f);
				float v2 = uValue(cache, x2, 0.0f);
				if(v1 == 0.0f)
				{
					cv::getRectSubPix(rightPyramid[0],
							winSize,
							cv::Point2f(x1, leftCorners[i].y),
							windowRight,
							windowRight.type());
					v1 = ssdApproach?ssd(windowLeft, windowRight):sad(windowLeft, windowRight);
				}
				if(v2 == 0.0f)
				{
					cv::getRectSubPix(rightPyramid[0],
							winSize,
							cv::Point2f(x2, leftCorners[i].y),
							windowRight,
							windowRight.type());
					v2 = ssdApproach?ssd(windowLeft, windowRight):sad(windowLeft, windowRight);
				}

				float previousXc = xc;
				float previousVc = vc;

				xc = v1<vc&&v1<v2?x1:v2<vc&&v2<v1?x2:xc;
				vc = v1<vc&&v1<v2?v1:v2<vc&&v2<v1?v2:vc;

				if(previousXc == xc)
				{
					step /= 2.0f;
				}
				else
				{
					cache.insert(std::make_pair(previousXc, previousVc));
				}

				if(xc < leftCorners[i].x+float(d)-1.0f || xc > leftCorners[i].x+float(d)+1.0f)
				{
					reject = true;
					break;
				}
			}

			rightCorners[i] = cv::Point2f(xc, leftCorners[i].y);
			status[i] = reject?0:1;
			if(!reject)
			{
				if(leftCorners[i].x+float(d) != xc)
				{
					++noSubPixel;
				}
				++added;
			}
		}
		subpixelTime+=timer.ticks();
	}
	UDEBUG("SubPixel=%d/%d added (total=%d)", noSubPixel, added, (int)status.size());
	UDEBUG("totalIterations=%d", totalIterations);
	UDEBUG("Time pyramid = %f s", pyramidTime);
	UDEBUG("Time disparity = %f s", disparityTime);
	UDEBUG("Time sub-pixel = %f s", subpixelTime);

	return rightCorners;
}

typedef float acctype;
typedef float itemtype;
#define  CV_DESCALE(x,n)     (((x) + (1 << ((n)-1))) >> (n))

//
// Adapted from OpenCV cv::calcOpticalFlowPyrLK() to force
// only optical flow on x-axis (assuming that prevImg is the left
// image and nextImg is the right image):
// https://github.com/Itseez/opencv/blob/ddf82d0b154873510802ef75c53e628cd7b2cb13/modules/video/src/lkpyramid.cpp#L1088
//
// The difference is on this line:
// https://github.com/Itseez/opencv/blob/ddf82d0b154873510802ef75c53e628cd7b2cb13/modules/video/src/lkpyramid.cpp#L683-L684
// - cv::Point2f delta( (float)((A12*b2 - A22*b1) * D), (float)((A12*b1 - A11*b2) * D));
// + cv::Point2f delta( (float)((A12*b2 - A22*b1) * D), 0); //<--- note the 0 for y
//
void calcOpticalFlowPyrLKStereo( cv::InputArray _prevImg, cv::InputArray _nextImg,
                           cv::InputArray _prevPts, cv::InputOutputArray _nextPts,
                           cv::OutputArray _status, cv::OutputArray _err,
                           cv::Size winSize, int maxLevel,
                           cv::TermCriteria criteria,
                           int flags, double minEigThreshold )
{
    cv::Mat prevPtsMat = _prevPts.getMat();
    const int derivDepth = cv::DataType<short>::depth;

    CV_Assert( maxLevel >= 0 && winSize.width > 2 && winSize.height > 2 );

    int level=0, i, npoints;
    CV_Assert( (npoints = prevPtsMat.checkVector(2, CV_32F, true)) >= 0 );

    if( npoints == 0 )
    {
        _nextPts.release();
        _status.release();
        _err.release();
        return;
    }

    if( !(flags & cv::OPTFLOW_USE_INITIAL_FLOW) )
        _nextPts.create(prevPtsMat.size(), prevPtsMat.type(), -1, true);

    cv::Mat nextPtsMat = _nextPts.getMat();
    CV_Assert( nextPtsMat.checkVector(2, CV_32F, true) == npoints );

    const cv::Point2f* prevPts = prevPtsMat.ptr<cv::Point2f>();
    cv::Point2f* nextPts = nextPtsMat.ptr<cv::Point2f>();

    _status.create((int)npoints, 1, CV_8U, -1, true);
    cv::Mat statusMat = _status.getMat(), errMat;
    CV_Assert( statusMat.isContinuous() );
    uchar* status = statusMat.ptr();
    float* err = 0;

    for( i = 0; i < npoints; i++ )
        status[i] = true;

    if( _err.needed() )
    {
        _err.create((int)npoints, 1, CV_32F, -1, true);
        errMat = _err.getMat();
        CV_Assert( errMat.isContinuous() );
        err = errMat.ptr<float>();
    }

    std::vector<cv::Mat> prevPyr, nextPyr;
    int levels1 = -1;
    int lvlStep1 = 1;
    int levels2 = -1;
    int lvlStep2 = 1;


    if(_prevImg.kind() != cv::_InputArray::STD_VECTOR_MAT)
	{
		//create pyramid
		maxLevel =  cv::buildOpticalFlowPyramid(_prevImg, prevPyr, winSize, maxLevel, true);
	}
	else if(_prevImg.kind() == cv::_InputArray::STD_VECTOR_MAT)
	{
		_prevImg.getMatVector(prevPyr);
	}

	levels1 = int(prevPyr.size()) - 1;
	CV_Assert(levels1 >= 0);

	if (levels1 % 2 == 1 && prevPyr[0].channels() * 2 == prevPyr[1].channels() && prevPyr[1].depth() == derivDepth)
	{
		lvlStep1 = 2;
		levels1 /= 2;
	}

	// ensure that pyramid has required padding
	if(levels1 > 0)
	{
		cv::Size fullSize;
		cv::Point ofs;
		prevPyr[lvlStep1].locateROI(fullSize, ofs);
		CV_Assert(ofs.x >= winSize.width && ofs.y >= winSize.height
			&& ofs.x + prevPyr[lvlStep1].cols + winSize.width <= fullSize.width
			&& ofs.y + prevPyr[lvlStep1].rows + winSize.height <= fullSize.height);
	}

	if(levels1 < maxLevel)
		maxLevel = levels1;

	if(_nextImg.kind() != cv::_InputArray::STD_VECTOR_MAT)
	{
		//create pyramid
		maxLevel =  cv::buildOpticalFlowPyramid(_nextImg, nextPyr, winSize, maxLevel, false);
	}
	else if(_nextImg.kind() == cv::_InputArray::STD_VECTOR_MAT)
    {
        _nextImg.getMatVector(nextPyr);
    }

	levels2 = int(nextPyr.size()) - 1;
	CV_Assert(levels2 >= 0);

	if (levels2 % 2 == 1 && nextPyr[0].channels() * 2 == nextPyr[1].channels() && nextPyr[1].depth() == derivDepth)
	{
		lvlStep2 = 2;
		levels2 /= 2;
	}

	// ensure that pyramid has required padding
	if(levels2 > 0)
	{
		cv::Size fullSize;
		cv::Point ofs;
		nextPyr[lvlStep2].locateROI(fullSize, ofs);
		CV_Assert(ofs.x >= winSize.width && ofs.y >= winSize.height
			&& ofs.x + nextPyr[lvlStep2].cols + winSize.width <= fullSize.width
			&& ofs.y + nextPyr[lvlStep2].rows + winSize.height <= fullSize.height);
	}

	if(levels2 < maxLevel)
		maxLevel = levels2;

    if( (criteria.type & cv::TermCriteria::COUNT) == 0 )
        criteria.maxCount = 30;
    else
        criteria.maxCount = std::min(std::max(criteria.maxCount, 0), 100);
    if( (criteria.type & cv::TermCriteria::EPS) == 0 )
        criteria.epsilon = 0.01;
    else
        criteria.epsilon = std::min(std::max(criteria.epsilon, 0.), 10.);
    criteria.epsilon *= criteria.epsilon;

    // for all pyramids
    for( level = maxLevel; level >= 0; level-- )
    {
        cv::Mat derivI = prevPyr[level * lvlStep1 + 1];

        CV_Assert(prevPyr[level * lvlStep1].size() == nextPyr[level * lvlStep2].size());
        CV_Assert(prevPyr[level * lvlStep1].type() == nextPyr[level * lvlStep2].type());

        const cv::Mat & prevImg = prevPyr[level * lvlStep1];
        const cv::Mat & prevDeriv = derivI;
        const cv::Mat & nextImg = nextPyr[level * lvlStep2];

        // for all corners
        {
        	cv::Point2f halfWin((winSize.width-1)*0.5f, (winSize.height-1)*0.5f);
			const cv::Mat& I = prevImg;
			const cv::Mat& J = nextImg;
			const cv::Mat& derivI = prevDeriv;

			int j, cn = I.channels(), cn2 = cn*2;
			cv::AutoBuffer<short> _buf(winSize.area()*(cn + cn2));
			int derivDepth = cv::DataType<short>::depth;

			cv::Mat IWinBuf(winSize, CV_MAKETYPE(derivDepth, cn), (short*)_buf);
			cv::Mat derivIWinBuf(winSize, CV_MAKETYPE(derivDepth, cn2), (short*)_buf + winSize.area()*cn);

			for( int ptidx = 0; ptidx < npoints; ptidx++ )
			{
				cv::Point2f prevPt = prevPts[ptidx]*(float)(1./(1 << level));
				cv::Point2f nextPt;
				if( level == maxLevel )
				{
					if( flags & cv::OPTFLOW_USE_INITIAL_FLOW )
						nextPt = nextPts[ptidx]*(float)(1./(1 << level));
					else
						nextPt = prevPt;
				}
				else
					nextPt = nextPts[ptidx]*2.f;
				nextPts[ptidx] = nextPt;

				cv::Point2i iprevPt, inextPt;
				prevPt -= halfWin;
				iprevPt.x = cvFloor(prevPt.x);
				iprevPt.y = cvFloor(prevPt.y);

				if( iprevPt.x < -winSize.width || iprevPt.x >= derivI.cols ||
					iprevPt.y < -winSize.height || iprevPt.y >= derivI.rows )
				{
					if( level == 0 )
					{
						if( status )
							status[ptidx] = false;
						if( err )
							err[ptidx] = 0;
					}
					continue;
				}

				float a = prevPt.x - iprevPt.x;
				float b = prevPt.y - iprevPt.y;
				const int W_BITS = 14, W_BITS1 = 14;
				const float FLT_SCALE = 1.f/(1 << 20);
				int iw00 = cvRound((1.f - a)*(1.f - b)*(1 << W_BITS));
				int iw01 = cvRound(a*(1.f - b)*(1 << W_BITS));
				int iw10 = cvRound((1.f - a)*b*(1 << W_BITS));
				int iw11 = (1 << W_BITS) - iw00 - iw01 - iw10;

				int dstep = (int)(derivI.step/derivI.elemSize1());
				int stepI = (int)(I.step/I.elemSize1());
				int stepJ = (int)(J.step/J.elemSize1());
				acctype iA11 = 0, iA12 = 0, iA22 = 0;
				float A11, A12, A22;

				// extract the patch from the first image, compute covariation cv::Matrix of derivatives
				int x, y;
				for( y = 0; y < winSize.height; y++ )
				{
					const uchar* src = I.ptr() + (y + iprevPt.y)*stepI + iprevPt.x*cn;
					const short* dsrc = derivI.ptr<short>() + (y + iprevPt.y)*dstep + iprevPt.x*cn2;

					short* Iptr = IWinBuf.ptr<short>(y);
					short* dIptr = derivIWinBuf.ptr<short>(y);

					x = 0;

					for( ; x < winSize.width*cn; x++, dsrc += 2, dIptr += 2 )
					{
						int ival = CV_DESCALE(src[x]*iw00 + src[x+cn]*iw01 +
											  src[x+stepI]*iw10 + src[x+stepI+cn]*iw11, W_BITS1-5);
						int ixval = CV_DESCALE(dsrc[0]*iw00 + dsrc[cn2]*iw01 +
											   dsrc[dstep]*iw10 + dsrc[dstep+cn2]*iw11, W_BITS1);
						int iyval = CV_DESCALE(dsrc[1]*iw00 + dsrc[cn2+1]*iw01 + dsrc[dstep+1]*iw10 +
											   dsrc[dstep+cn2+1]*iw11, W_BITS1);

						Iptr[x] = (short)ival;
						dIptr[0] = (short)ixval;
						dIptr[1] = (short)iyval;

						iA11 += (itemtype)(ixval*ixval);
						iA12 += (itemtype)(ixval*iyval);
						iA22 += (itemtype)(iyval*iyval);
					}
				}

				A11 = iA11*FLT_SCALE;
				A12 = iA12*FLT_SCALE;
				A22 = iA22*FLT_SCALE;

				float D = A11*A22 - A12*A12;
				float minEig = (A22 + A11 - std::sqrt((A11-A22)*(A11-A22) +
								4.f*A12*A12))/(2*winSize.width*winSize.height);

				if( err && (flags & cv::OPTFLOW_LK_GET_MIN_EIGENVALS) != 0 )
					err[ptidx] = (float)minEig;

				if( minEig < minEigThreshold || D < FLT_EPSILON )
				{
					if( level == 0 && status )
						status[ptidx] = false;
					continue;
				}

				D = 1.f/D;

				nextPt -= halfWin;
				cv::Point2f prevDelta;

				for( j = 0; j < criteria.maxCount; j++ )
				{
					inextPt.x = cvFloor(nextPt.x);
					inextPt.y = cvFloor(nextPt.y);

					if( inextPt.x < -winSize.width || inextPt.x >= J.cols ||
					   inextPt.y < -winSize.height || inextPt.y >= J.rows )
					{
						if( level == 0 && status )
							status[ptidx] = false;
						break;
					}

					a = nextPt.x - inextPt.x;
					b = nextPt.y - inextPt.y;
					iw00 = cvRound((1.f - a)*(1.f - b)*(1 << W_BITS));
					iw01 = cvRound(a*(1.f - b)*(1 << W_BITS));
					iw10 = cvRound((1.f - a)*b*(1 << W_BITS));
					iw11 = (1 << W_BITS) - iw00 - iw01 - iw10;
					acctype ib1 = 0, ib2 = 0;
					float b1, b2;

					for( y = 0; y < winSize.height; y++ )
					{
						const uchar* Jptr = J.ptr() + (y + inextPt.y)*stepJ + inextPt.x*cn;
						const short* Iptr = IWinBuf.ptr<short>(y);
						const short* dIptr = derivIWinBuf.ptr<short>(y);

						x = 0;

						for( ; x < winSize.width*cn; x++, dIptr += 2 )
						{
							int diff = CV_DESCALE(Jptr[x]*iw00 + Jptr[x+cn]*iw01 +
												  Jptr[x+stepJ]*iw10 + Jptr[x+stepJ+cn]*iw11,
												  W_BITS1-5) - Iptr[x];
							ib1 += (itemtype)(diff*dIptr[0]);
							ib2 += (itemtype)(diff*dIptr[1]);
						}
					}

					b1 = ib1*FLT_SCALE;
					b2 = ib2*FLT_SCALE;

					cv::Point2f delta( (float)((A12*b2 - A22*b1) * D),
								  0);//(float)((A12*b1 - A11*b2) * D)); // MODIFICATION
					//delta = -delta;

					nextPt += delta;
					nextPts[ptidx] = nextPt + halfWin;

					if( delta.ddot(delta) <= criteria.epsilon )
						break;

					if( j > 0 && std::abs(delta.x + prevDelta.x) < 0.01 &&
					   std::abs(delta.y + prevDelta.y) < 0.01 )
					{
						nextPts[ptidx] -= delta*0.5f;
						break;
					}
					prevDelta = delta;
				}

				if( status[ptidx] && err && level == 0 && (flags & cv::OPTFLOW_LK_GET_MIN_EIGENVALS) == 0 )
				{
					cv::Point2f nextPoint = nextPts[ptidx] - halfWin;
					cv::Point inextPoint;

					inextPoint.x = cvFloor(nextPoint.x);
					inextPoint.y = cvFloor(nextPoint.y);

					if( inextPoint.x < -winSize.width || inextPoint.x >= J.cols ||
						inextPoint.y < -winSize.height || inextPoint.y >= J.rows )
					{
						if( status )
							status[ptidx] = false;
						continue;
					}

					float aa = nextPoint.x - inextPoint.x;
					float bb = nextPoint.y - inextPoint.y;
					iw00 = cvRound((1.f - aa)*(1.f - bb)*(1 << W_BITS));
					iw01 = cvRound(aa*(1.f - bb)*(1 << W_BITS));
					iw10 = cvRound((1.f - aa)*bb*(1 << W_BITS));
					iw11 = (1 << W_BITS) - iw00 - iw01 - iw10;
					float errval = 0.f;

					for( y = 0; y < winSize.height; y++ )
					{
						const uchar* Jptr = J.ptr() + (y + inextPoint.y)*stepJ + inextPoint.x*cn;
						const short* Iptr = IWinBuf.ptr<short>(y);

						for( x = 0; x < winSize.width*cn; x++ )
						{
							int diff = CV_DESCALE(Jptr[x]*iw00 + Jptr[x+cn]*iw01 +
												  Jptr[x+stepJ]*iw10 + Jptr[x+stepJ+cn]*iw11,
												  W_BITS1-5) - Iptr[x];
							errval += std::abs((float)diff);
						}
					}
					err[ptidx] = errval * 1.f/(32*winSize.width*cn*winSize.height);
				}
			}
        }

    }
}

cv::Mat disparityFromStereoImages(
		const cv::Mat & leftImage,
		const cv::Mat & rightImage,
		int type)
{
	UASSERT(!leftImage.empty() && !rightImage.empty());
	UASSERT(leftImage.cols == rightImage.cols && leftImage.rows == rightImage.rows);
	UASSERT((leftImage.type() == CV_8UC1 || leftImage.type() == CV_8UC3) && rightImage.type() == CV_8UC1);
	UASSERT(type == CV_32FC1 || type == CV_16SC1);

	cv::Mat leftMono;
	if(leftImage.channels() == 3)
	{
		cv::cvtColor(leftImage, leftMono, CV_BGR2GRAY);
	}
	else
	{
		leftMono = leftImage;
	}
	cv::Mat disparity;
#if CV_MAJOR_VERSION < 3
	cv::StereoBM stereo(cv::StereoBM::BASIC_PRESET);
	stereo.state->SADWindowSize = 15;
	stereo.state->minDisparity = 0;
	stereo.state->numberOfDisparities = 64;
	stereo.state->preFilterSize = 9;
	stereo.state->preFilterCap = 31;
	stereo.state->uniquenessRatio = 15;
	stereo.state->textureThreshold = 10;
	stereo.state->speckleWindowSize = 100;
	stereo.state->speckleRange = 4;
	stereo(leftMono, rightImage, disparity, type);
#else
	cv::Ptr<cv::StereoBM> stereo = cv::StereoBM::create();
	stereo->setBlockSize(15);
	stereo->setMinDisparity(0);
	stereo->setNumDisparities(64);
	stereo->setPreFilterSize(9);
	stereo->setPreFilterCap(31);
	stereo->setUniquenessRatio(15);
	stereo->setTextureThreshold(10);
	stereo->setSpeckleWindowSize(100);
	stereo->setSpeckleRange(4);
	stereo->compute(leftMono, rightImage, disparity);
#endif
	return disparity;
}

cv::Mat depthFromDisparity(const cv::Mat & disparity,
		float fx, float baseline,
		int type)
{
	UASSERT(!disparity.empty() && (disparity.type() == CV_32FC1 || disparity.type() == CV_16SC1));
	UASSERT(type == CV_32FC1 || type == CV_16UC1);
	cv::Mat depth = cv::Mat::zeros(disparity.rows, disparity.cols, type);
	int countOverMax = 0;
	for (int i = 0; i < disparity.rows; i++)
	{
		for (int j = 0; j < disparity.cols; j++)
		{
			float disparity_value = disparity.type() == CV_16SC1?float(disparity.at<short>(i,j))/16.0f:disparity.at<float>(i,j);
			if (disparity_value > 0.0f)
			{
				// baseline * focal / disparity
				float d = baseline * fx / disparity_value;
				if(d>0)
				{
					if(depth.type() == CV_32FC1)
					{
						depth.at<float>(i,j) = d;
					}
					else
					{
						if(d*1000.0f <= (float)USHRT_MAX)
						{
							depth.at<unsigned short>(i,j) = (unsigned short)(d*1000.0f);
						}
						else
						{
							++countOverMax;
						}
					}
				}
			}
		}
	}
	if(countOverMax)
	{
		UWARN("Depth conversion error, %d depth values ignored because they are over the maximum depth allowed (65535 mm).", countOverMax);
	}
	return depth;
}

cv::Mat depthFromStereoImages(
		const cv::Mat & leftImage,
		const cv::Mat & rightImage,
		const std::vector<cv::Point2f> & leftCorners,
		float fx,
		float baseline,
		int flowWinSize,
		int flowMaxLevel,
		int flowIterations,
		double flowEps)
{
	UASSERT(!leftImage.empty() && !rightImage.empty() &&
			leftImage.type() == CV_8UC1 && rightImage.type() == CV_8UC1 &&
			leftImage.cols == rightImage.cols &&
			leftImage.rows == rightImage.rows);
	UASSERT(fx > 0.0f && baseline > 0.0f);

	// Find features in the new left image
	std::vector<unsigned char> status;
	std::vector<float> err;
	std::vector<cv::Point2f> rightCorners;
	UDEBUG("cv::calcOpticalFlowPyrLK() begin");
	cv::calcOpticalFlowPyrLK(
			leftImage,
			rightImage,
			leftCorners,
			rightCorners,
			status,
			err,
			cv::Size(flowWinSize, flowWinSize), flowMaxLevel,
			cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS, flowIterations, flowEps),
			cv::OPTFLOW_LK_GET_MIN_EIGENVALS, 1e-4);
	UDEBUG("cv::calcOpticalFlowPyrLK() end");

	return depthFromStereoCorrespondences(leftImage, leftCorners, rightCorners, status, fx, baseline);
}

cv::Mat disparityFromStereoCorrespondences(
		const cv::Size & disparitySize,
		const std::vector<cv::Point2f> & leftCorners,
		const std::vector<cv::Point2f> & rightCorners,
		const std::vector<unsigned char> & mask)
{
	UASSERT(leftCorners.size() == rightCorners.size());
	UASSERT(mask.size() == 0 || mask.size() == leftCorners.size());
	cv::Mat disparity = cv::Mat::zeros(disparitySize, CV_32FC1);
	for(unsigned int i=0; i<leftCorners.size(); ++i)
	{
		if(mask.empty() || mask[i])
		{
			cv::Point2i dispPt(int(leftCorners[i].y+0.5f), int(leftCorners[i].x+0.5f));
			UASSERT(dispPt.x >= 0 && dispPt.x < disparitySize.width);
			UASSERT(dispPt.y >= 0 && dispPt.y < disparitySize.height);
			disparity.at<float>(dispPt.y, dispPt.x) = leftCorners[i].x - rightCorners[i].x;
		}
	}
	return disparity;
}

cv::Mat depthFromStereoCorrespondences(
		const cv::Mat & leftImage,
		const std::vector<cv::Point2f> & leftCorners,
		const std::vector<cv::Point2f> & rightCorners,
		const std::vector<unsigned char> & mask,
		float fx, float baseline)
{
	UASSERT(!leftImage.empty() && leftCorners.size() == rightCorners.size());
	UASSERT(mask.size() == 0 || mask.size() == leftCorners.size());
	cv::Mat depth = cv::Mat::zeros(leftImage.rows, leftImage.cols, CV_32FC1);
	for(unsigned int i=0; i<leftCorners.size(); ++i)
	{
		if(mask.size() == 0 || mask[i])
		{
			float disparity = leftCorners[i].x - rightCorners[i].x;
			if(disparity > 0.0f)
			{
				float d = baseline * fx / disparity;
				depth.at<float>(int(leftCorners[i].y+0.5f), int(leftCorners[i].x+0.5f)) = d;
			}
		}
	}
	return depth;
}

cv::Mat cvtDepthFromFloat(const cv::Mat & depth32F)
{
	UASSERT(depth32F.empty() || depth32F.type() == CV_32FC1);
	cv::Mat depth16U;
	if(!depth32F.empty())
	{
		depth16U = cv::Mat(depth32F.rows, depth32F.cols, CV_16UC1);
		int countOverMax = 0;
		for(int i=0; i<depth32F.rows; ++i)
		{
			for(int j=0; j<depth32F.cols; ++j)
			{
				float depth = (depth32F.at<float>(i,j)*1000.0f);
				unsigned short depthMM = 0;
				if(depth > 0 && depth <= (float)USHRT_MAX)
				{
					depthMM = (unsigned short)depth;
				}
				else if(depth > (float)USHRT_MAX)
				{
					++countOverMax;
				}
				depth16U.at<unsigned short>(i, j) = depthMM;
			}
		}
		if(countOverMax)
		{
			UWARN("Depth conversion error, %d depth values ignored because "
				  "they are over the maximum depth allowed (65535 mm). Is the depth "
				  "image really in meters? 32 bits images should be in meters, "
				  "and 16 bits should be in mm.", countOverMax);
		}
	}
	return depth16U;
}

cv::Mat cvtDepthToFloat(const cv::Mat & depth16U)
{
	UASSERT(depth16U.empty() || depth16U.type() == CV_16UC1);
	cv::Mat depth32F;
	if(!depth16U.empty())
	{
		depth32F = cv::Mat(depth16U.rows, depth16U.cols, CV_32FC1);
		for(int i=0; i<depth16U.rows; ++i)
		{
			for(int j=0; j<depth16U.cols; ++j)
			{
				float depth = float(depth16U.at<unsigned short>(i,j))/1000.0f;
				depth32F.at<float>(i, j) = depth;
			}
		}
	}
	return depth32F;
}

float getDepth(
		const cv::Mat & depthImage,
		float x, float y,
		bool smoothing,
		float maxZError)
{
	UASSERT(!depthImage.empty());
	UASSERT(depthImage.type() == CV_16UC1 || depthImage.type() == CV_32FC1);

	int u = int(x+0.5f);
	int v = int(y+0.5f);

	if(!(u >=0 && u<depthImage.cols && v >=0 && v<depthImage.rows))
	{
		UDEBUG("!(x >=0 && x<depthImage.cols && y >=0 && y<depthImage.rows) cond failed! returning bad point. (x=%f (u=%d), y=%f (v=%d), cols=%d, rows=%d)",
				x,u,y,v,depthImage.cols, depthImage.rows);
		return 0;
	}

	bool isInMM = depthImage.type() == CV_16UC1; // is in mm?

	// Inspired from RGBDFrame::getGaussianMixtureDistribution() method from
	// https://github.com/ccny-ros-pkg/rgbdtools/blob/master/src/rgbd_frame.cpp
	// Window weights:
	//  | 1 | 2 | 1 |
	//  | 2 | 4 | 2 |
	//  | 1 | 2 | 1 |
	int u_start = std::max(u-1, 0);
	int v_start = std::max(v-1, 0);
	int u_end = std::min(u+1, depthImage.cols-1);
	int v_end = std::min(v+1, depthImage.rows-1);

	float depth = isInMM?(float)depthImage.at<unsigned short>(v,u)*0.001f:depthImage.at<float>(v,u);
	if(depth!=0.0f && uIsFinite(depth))
	{
		if(smoothing)
		{
			float sumWeights = 0.0f;
			float sumDepths = 0.0f;
			for(int uu = u_start; uu <= u_end; ++uu)
			{
				for(int vv = v_start; vv <= v_end; ++vv)
				{
					if(!(uu == u && vv == v))
					{
						float d = isInMM?(float)depthImage.at<unsigned short>(vv,uu)*0.001f:depthImage.at<float>(vv,uu);
						// ignore if not valid or depth difference is too high
						if(d != 0.0f && uIsFinite(d) && fabs(d - depth) < maxZError)
						{
							if(uu == u || vv == v)
							{
								sumWeights+=2.0f;
								d*=2.0f;
							}
							else
							{
								sumWeights+=1.0f;
							}
							sumDepths += d;
						}
					}
				}
			}
			// set window weight to center point
			depth *= 4.0f;
			sumWeights += 4.0f;

			// mean
			depth = (depth+sumDepths)/sumWeights;
		}
	}
	else
	{
		depth = 0;
	}
	return depth;
}

cv::Mat decimate(const cv::Mat & image, int decimation)
{
	UASSERT(decimation >= 1);
	cv::Mat out;
	if(!image.empty())
	{
		if(decimation > 1)
		{
			if((image.type() == CV_32FC1 || image.type()==CV_16UC1))
			{
				UASSERT_MSG(image.rows % decimation == 0 && image.cols % decimation == 0, "Decimation of depth images should be exact!");

				out = cv::Mat(image.rows/decimation, image.cols/decimation, image.type());
				if(image.type() == CV_32FC1)
				{
					for(int j=0; j<out.rows; ++j)
					{
						for(int i=0; i<out.cols; ++i)
						{
							out.at<float>(j, i) = image.at<float>(j*decimation, i*decimation);
						}
					}
				}
				else // CV_16UC1
				{
					for(int j=0; j<out.rows; ++j)
					{
						for(int i=0; i<out.cols; ++i)
						{
							out.at<unsigned short>(j, i) = image.at<unsigned short>(j*decimation, i*decimation);
						}
					}
				}
			}
			else
			{
				cv::resize(image, out, cv::Size(), 1.0f/float(decimation), 1.0f/float(decimation), cv::INTER_AREA);
			}
		}
		else
		{
			out = image;
		}
	}
	return out;
}

// Registration Depth to RGB
cv::Mat registerDepth(
		const cv::Mat & depth,
		const cv::Mat & depthK,
		const cv::Mat & colorK,
		const rtabmap::Transform & transform)
{
	UASSERT(!transform.isNull());
	UASSERT(!depth.empty());
	UASSERT(depth.type() == CV_16UC1); // mm
	UASSERT(depthK.type() == CV_64FC1 && depthK.cols == 3 && depthK.cols == 3);
	UASSERT(colorK.type() == CV_64FC1 && colorK.cols == 3 && colorK.cols == 3);

	float fx = depthK.at<double>(0,0);
	float fy = depthK.at<double>(1,1);
	float cx = depthK.at<double>(0,2);
	float cy = depthK.at<double>(1,2);

	float rfx = colorK.at<double>(0,0);
	float rfy = colorK.at<double>(1,1);
	float rcx = colorK.at<double>(0,2);
	float rcy = colorK.at<double>(1,2);

	Eigen::Affine3f proj = transform.toEigen3f();
	Eigen::Vector4f P4,P3;
	P4[3] = 1;
	cv::Mat registered = cv::Mat::zeros(depth.rows, depth.cols, depth.type());

	for(int y=0; y<depth.rows; ++y)
	{
		for(int x=0; x<depth.cols; ++x)
		{
			//filtering
			float dz = float(depth.at<unsigned short>(y,x))*0.001f; // put in meter for projection
			if(dz>=0.0f)
			{
				// Project to 3D
				P4[0] = (x - cx) * dz / fx; // Optimization: we could have (x-cx)/fx in a lookup table
				P4[1] = (y - cy) * dz / fy; // Optimization: we could have (y-cy)/fy in a lookup table
				P4[2] = dz;

				P3 = proj * P4;
				float z = P3[2];
				float invZ = 1.0f/z;
				int dx = (rfx*P3[0])*invZ + rcx;
				int dy = (rfy*P3[1])*invZ + rcy;

				if(uIsInBounds(dx, 0, registered.cols) && uIsInBounds(dy, 0, registered.rows))
				{
					unsigned short z16 = z * 1000; //mm
					unsigned short &zReg = registered.at<unsigned short>(dy, dx);
					if(zReg == 0 || z16 < zReg)
					{
						zReg = z16;
					}
				}
			}
		}
	}
	return registered;
}

void fillRegisteredDepthHoles(cv::Mat & registeredDepth, bool vertical, bool horizontal, bool fillDoubleHoles)
{
	UASSERT(registeredDepth.type() == CV_16UC1);
	int margin = fillDoubleHoles?2:1;
	for(int x=1; x<registeredDepth.cols-margin; ++x)
	{
		for(int y=1; y<registeredDepth.rows-margin; ++y)
		{
			unsigned short & b = registeredDepth.at<unsigned short>(y, x);
			bool set = false;
			if(vertical)
			{
				const unsigned short & a = registeredDepth.at<unsigned short>(y-1, x);
				unsigned short & c = registeredDepth.at<unsigned short>(y+1, x);
				if(a && c)
				{
					unsigned short error = 0.01*((a+c)/2);
					if(((b == 0 && a && c) || (b > a+error && b > c+error)) &&
						(a>c?a-c<=error:c-a<=error))
					{
						b = (a+c)/2;
						set = true;
						if(!horizontal)
						{
							++y;
						}
					}
				}
				if(!set && fillDoubleHoles)
				{
					const unsigned short & d = registeredDepth.at<unsigned short>(y+2, x);
					if(a && d && (b==0 || c==0))
					{
						unsigned short error = 0.01*((a+d)/2);
						if(((b == 0 && a && d) || (b > a+error && b > d+error)) &&
						   ((c == 0 && a && d) || (c > a+error && c > d+error)) &&
							(a>d?a-d<=error:d-a<=error))
						{
							if(a>d)
							{
								unsigned short tmp = (a-d)/4;
								b = d + tmp;
								c = d + 3*tmp;
							}
							else
							{
								unsigned short tmp = (d-a)/4;
								b = a + tmp;
								c = a + 3*tmp;
							}
							set = true;
							if(!horizontal)
							{
								y+=2;
							}
						}
					}
				}
			}
			if(!set && horizontal)
			{
				const unsigned short & a = registeredDepth.at<unsigned short>(y, x-1);
				unsigned short & c = registeredDepth.at<unsigned short>(y, x+1);
				if(a && c)
				{
					unsigned short error = 0.01*((a+c)/2);
					if(((b == 0 && a && c) || (b > a+error && b > c+error)) &&
						(a>c?a-c<=error:c-a<=error))
					{
						b = (a+c)/2;
						set = true;
					}
				}
				if(!set && fillDoubleHoles)
				{
					const unsigned short & d = registeredDepth.at<unsigned short>(y, x+2);
					if(a && d && (b==0 || c==0))
					{
						unsigned short error = 0.01*((a+d)/2);
						if(((b == 0 && a && d) || (b > a+error && b > d+error)) &&
						   ((c == 0 && a && d) || (c > a+error && c > d+error)) &&
							(a>d?a-d<=error:d-a<=error))
						{
							if(a>d)
							{
								unsigned short tmp = (a-d)/4;
								b = d + tmp;
								c = d + 3*tmp;
							}
							else
							{
								unsigned short tmp = (d-a)/4;
								b = a + tmp;
								c = a + 3*tmp;
							}
						}
					}
				}
			}
		}
	}
}

}

}
