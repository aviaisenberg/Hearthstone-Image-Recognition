#include "Recognizer.h"
#include "SystemInterface.h"

#include <stdio.h>
#include <iostream>
#include <algorithm>

#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/timer.hpp>
#include <boost/progress.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/assign/std/vector.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/assign/list_inserter.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>

using namespace boost::assign;

namespace hs {

Recognizer::Recognizer(DatabasePtr db, std::string calibrationID) {
	this->db = db;
	auto cfg = Config::getConfig();
	phashThreshold = cfg.get<int>("config.image_recognition.phash_threshold");

	c = CalibrationPtr(new Calibration(cfg.get<std::string>("config.paths.calibrations_path") + "/" + calibrationID + ".xml"));
	if (!c->valid) {
		HS_ERROR << "Calibration with ID " << calibrationID << " was not properly initialized, trying to use default..." << std::endl;
		c = CalibrationPtr(new Calibration(cfg.get<std::string>("config.paths.calibrations_path") + "/default.xml"));
	}

	if (db->hasMissingData()) {
		HS_INFO << "pHashes missing from database, filling..." << std::endl;
		precomputeData();
	}

    for (auto& e : db->cards) {
		DataSetEntry o(e.id);
		setCards.entries.push_back(o);
		setCards.hashes.push_back(e.phash);
    }

    for (auto& e : db->heroes) {
		DataSetEntry o(e.id);
		setClasses.entries.push_back(o);
		setClasses.hashes.push_back(e.phash);
    }

	surf = cv::SURF(100, 2, 2, true, true);
    HS_INFO << "Using SURF parameters: " << surf.hessianThreshold << " " << surf.nOctaves << " " << surf.nOctaveLayers << " " << surf.extended << " " << surf.upright << std::endl;
	matcher = BFMatcherPtr(new cv::BFMatcher(cv::NORM_L2));

    cv::Mat tempImg;
    tempImg = cv::imread(cfg.get<std::string>("config.paths.misc_image_path") + "/" + "game_end_victory.png", CV_LOAD_IMAGE_GRAYSCALE);
    descriptorEnd.push_back(std::make_pair(getDescriptor(tempImg), RESULT_GAME_END_VICTORY));
    tempImg = cv::imread(cfg.get<std::string>("config.paths.misc_image_path") + "/" + "game_end_defeat.png", CV_LOAD_IMAGE_GRAYSCALE);
    descriptorEnd.push_back(std::make_pair(getDescriptor(tempImg), RESULT_GAME_END_DEFEAT));

    tempImg = cv::imread(cfg.get<std::string>("config.paths.misc_image_path") + "/" + "game_coin_first.png", CV_LOAD_IMAGE_GRAYSCALE);
    descriptorCoin.push_back(std::make_pair(getDescriptor(tempImg), RESULT_GAME_COIN_FIRST));
    tempImg = cv::imread(cfg.get<std::string>("config.paths.misc_image_path") + "/" + "game_coin_second.png", CV_LOAD_IMAGE_GRAYSCALE);
    descriptorCoin.push_back(std::make_pair(getDescriptor(tempImg), RESULT_GAME_COIN_SECOND));

    //set each set's phash threshold
    setCards.phashThreshold = phashThreshold;
    setClasses.phashThreshold = phashThreshold;

    //declare recognizers
    phashRecognizers.push_back(boost::make_tuple(RECOGNIZER_DRAFT_CLASS_PICK, c->roiDraftClassPick, setClasses));
    phashRecognizers.push_back(boost::make_tuple(RECOGNIZER_DRAFT_CARD_PICK, c->roiDraftCardPick, setCards));
    phashRecognizers.push_back(boost::make_tuple(RECOGNIZER_GAME_CLASS_SHOW, c->roiGameClassShow, setClasses));
    phashRecognizers.push_back(boost::make_tuple(RECOGNIZER_GAME_DRAW, c->roiGameDraw, setCards));
    phashRecognizers.push_back(boost::make_tuple(RECOGNIZER_GAME_DRAW_INIT_1, c->roiGameDrawInit1, setCards));
    phashRecognizers.push_back(boost::make_tuple(RECOGNIZER_GAME_DRAW_INIT_2, c->roiGameDrawInit2, setCards));

    surfRecognizers.push_back(boost::make_tuple(RECOGNIZER_GAME_COIN, c->roiGameCoin, descriptorCoin));
    surfRecognizers.push_back(boost::make_tuple(RECOGNIZER_GAME_END, c->roiGameEnd, descriptorEnd));
}

void Recognizer::precomputeData() {
	const std::string cardImagePath = Config::getConfig().get<std::string>("config.paths.card_image_path") + "/";
	const std::string heroImagePath = Config::getConfig().get<std::string>("config.paths.hero_image_path") + "/";

    for (auto& c : db->cards) {
    	std::string stringID = (boost::format("%03d") % c.id).str();
    	cv::Mat image = cv::imread(cardImagePath + stringID + ".png", CV_LOAD_IMAGE_GRAYSCALE);
    	c.phash = PerceptualHash::phash(image);
    }

    for (auto& h : db->heroes) {
    	std::string stringID = (boost::format("%03d") % h.id).str();
    	cv::Mat image = cv::imread(heroImagePath + stringID + ".png", CV_LOAD_IMAGE_GRAYSCALE);
    	h.phash = PerceptualHash::phash(image);
    }

    db->save();
}

std::vector<Recognizer::RecognitionResult> Recognizer::recognize(const cv::Mat& source, unsigned int allowedRecognizers) {
	std::vector<RecognitionResult> results;
	cv::Mat image;
	if (source.cols != c->res.width || source.rows != c->res.height) {
		cv::resize(source, image, cv::Size(c->res.width, c->res.height));
	} else {
		image = source;
	}

	for (const auto& rPHash : phashRecognizers) {
		if (allowedRecognizers & rPHash.get<0>()) {
			RecognitionResult rr = comparePHashes(image, rPHash.get<0>(), rPHash.get<1>(), rPHash.get<2>());
			if (rr.valid) results.push_back(rr);
		}
	}

	for (const auto& rSURF : surfRecognizers) {
		if (allowedRecognizers & rSURF.get<0>()) {
			RecognitionResult rr = compareFeatures(image, rSURF.get<0>(), rSURF.get<1>(), rSURF.get<2>());
			if (rr.valid) results.push_back(rr);
		}
	}

	if (allowedRecognizers & RECOGNIZER_DRAFT_CARD_PICK) {
		auto recognitionResult = std::find_if(results.begin(), results.end(), [&](const RecognitionResult& r) -> bool {
			return r.sourceRecognizer == RECOGNIZER_DRAFT_CARD_PICK;
		});
		if (recognitionResult != results.end()) lastDraftRecognition = recognitionResult->results;
	}

	return results;
}

Recognizer::RecognitionResult Recognizer::comparePHashes(const cv::Mat& image, unsigned int recognizer, const Calibration::VectorROI& roi, const DataSet& dataSet) {
	std::vector<DataSetEntry> bestMatches = bestPHashMatches(image, roi, dataSet);
	bool valid = true;
	for (auto& bestMatch : bestMatches) {
		valid &= bestMatch.valid;
	}
	RecognitionResult rr;
	rr.valid = valid;
	if (valid) {
		rr.sourceRecognizer = recognizer;
		for (DataSetEntry dse : bestMatches) {
			rr.results.push_back(dse.id);
		}
	}

	return rr;
}

std::vector<Recognizer::DataSetEntry> Recognizer::bestPHashMatches(const cv::Mat& image, const Calibration::VectorROI& roi, const DataSet& dataSet) {
	std::vector<DataSetEntry> results;
	for (auto& r : roi) {
		cv::Mat roiImage = image(
	    		cv::Range(r.y, r.y + r.height),
	    		cv::Range(r.x, r.x + r.width));
		ulong64 phash = PerceptualHash::phash(roiImage);
		PerceptualHash::ComparisonResult best = PerceptualHash::best(phash, dataSet.hashes);

		if (best.distance < dataSet.phashThreshold) {
			results.push_back(dataSet.entries[best.index]);
		} else {
			results.push_back(DataSetEntry());
		}
	}

	return results;
}

int Recognizer::getIndexOfBluest(const cv::Mat& image, const Calibration::VectorROI& roi) {
	if (lastDraftRecognition.empty()) return -1;
	int quality = db->cards[lastDraftRecognition[0]].quality;
	std::vector<float> hV;
	std::vector<float> sV;
	std::vector<float> vV;
	std::vector<std::vector<float> > hsv;
	for (auto& r : roi) {
		cv::Mat roiImage = image(
	    		cv::Range(r.y, r.y + r.height),
	    		cv::Range(r.x, r.x + r.width));
		cv::Mat hsvImage;
		cv::cvtColor(roiImage, hsvImage, CV_BGR2HSV);

		cv::Scalar means = cv::mean(hsvImage);
		hV.push_back(means[0]);
		sV.push_back(means[1]);
		vV.push_back(means[2]);
	}
	//hsv[0] == h of all slots, hsv[1][2] s value of third slot
	hsv.push_back(hV);
	hsv.push_back(sV);
	hsv.push_back(vV);

	int best = -1;
	int cand = -1;
//	HS_INFO << "(" << hsv[0][0] << "; " << hsv[0][1] << "; " << hsv[0][2] << ") (" << hsv[1][0] << "; " << hsv[1][1] << "; " << hsv[1][2] << ") (" << hsv[2][0] << "; " << hsv[2][1] << "; " << hsv[2][2] << ")" << std::endl;
	float averageValue = 1/3.0f * (hsv[2][0] + hsv[2][1] + hsv[2][2]);

	const bool averageAboveThres =
			((quality == 5) && averageValue >= 200) ||
			((quality != 5) && averageValue >= 220);
	if (!averageAboveThres) return best;

	std::vector<bool> red(3);
	std::vector<bool> candidateColor(3);
	bool match = true;
	size_t minSIndex = 0;
	float minS = hsv[1][0];

	//h value comparison
	for (size_t i = 0; i < hsv[0].size(); i++) {
		if (hsv[1][i] < minS) {
			minS = hsv[1][i];
			minSIndex = i;
		}
		const float h = hsv[0][i];
		const bool candidateEpic = (quality == 4) && 110 <= h && h <= 150;
		const bool candidateOther = (quality != 4) && ((90 <= h && h <= 110) || (50 <= h && h <= 80));
		red[i] = h < 30;
		candidateColor[i] = candidateEpic || candidateOther;
		//this is only true if there was another blue/green/purple candidate before
		if (cand >= 0 && candidateColor[i]) match = false;
		//chose a blue as candidate
		if (candidateColor[i]) cand = (int)i;
		//make sure the others are red
//		match &= (candidateColor[i] || red[i]);
	}
	if (match && minSIndex == cand) best = cand;

	return best;
}

Recognizer::RecognitionResult Recognizer::compareFeatures(const cv::Mat& image, unsigned int recognizer, const Calibration::VectorROI& roi, const VectorDescriptor& descriptors) {
    RecognitionResult rr;
    rr.valid = false;

	for (auto& r : roi) {
		cv::Mat roiImage = image(
	    		cv::Range(r.y, r.y + r.height),
	    		cv::Range(r.x, r.x + r.width));
	    cv::Mat greyscaleImage;
	    if (roiImage.channels() == 1) {
	    	greyscaleImage = roiImage.clone();
	    } else {
	    	cv::cvtColor(roiImage, greyscaleImage, CV_BGR2GRAY);
	    }

	    cv::Mat descriptorImage = getDescriptor(greyscaleImage);

	    int bestResultMatchesCount = 0;
	    int bestResult = -1;

	    if (!descriptorImage.data) continue;

	    std::vector<cv::DMatch> matches;
	    for (size_t i = 0; i < descriptors.size(); i++) {
	    	matches = getMatches(descriptorImage, descriptors[i].first);
	    	if (isGoodDescriptorMatch(matches) && matches.size() > bestResultMatchesCount) {
	    		bestResult = descriptors[i].second;
	    		bestResultMatchesCount = matches.size();
	    	}
	    }
	    if (bestResult != -1) {
    		rr.results.push_back(bestResult);
			rr.valid = true;
			rr.sourceRecognizer = recognizer;
	    }
	}

	return rr;
}

cv::Mat Recognizer::getDescriptor(cv::Mat& image) {
	std::vector<cv::KeyPoint> keypoints;
	cv::Mat descriptor;
	surf(image, cv::Mat(), keypoints, descriptor);
	return descriptor;
}

bool Recognizer::isGoodDescriptorMatch(const std::vector<cv::DMatch>& matches) {
	return matches.size() >= 7;
}

std::vector<cv::DMatch> Recognizer::getMatches(const cv::Mat& descriptorObj, const cv::Mat& descriptorScene) {
	std::vector<std::vector<cv::DMatch> > matches;
	matcher->knnMatch(descriptorObj, descriptorScene, matches, 2);
	std::vector<cv::DMatch> goodMatches;

	//ratio test
	for (size_t i = 0; i < matches.size(); ++i) {
		if (matches[i].size() < 2)
				continue;

		const cv::DMatch &m1 = matches[i][0];
		const cv::DMatch &m2 = matches[i][1];

		if (m1.distance <= 0.6f * m2.distance) {
			goodMatches.push_back(m1);
		}
	}

	return goodMatches;
}

}
