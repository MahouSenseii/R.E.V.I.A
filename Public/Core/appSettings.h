#pragma once

#include "Agents/conversationSettings.h"
#include "Agents/inputArbiterSettings.h"
#include "Agents/responseFilterSettings.h"
#include "Computer/computerSettings.h"
#include "Improvement/improvementSettings.h"
#include "Initiative/initiativeSettings.h"
#include "Intelligence/intelligenceSettings.h"
#include "LLM/endpointSettings.h"
#include "Perception/perceptionSettings.h"
#include "Performance/performanceSettings.h"
#include "Presence/presenceSettings.h"
#include "Resources/resourceSettings.h"
#include "Runtime/channelSettings.h"
#include "Speech/bargeInSettings.h"
#include "Speech/recognitionSettings.h"
#include "Speech/speechSettings.h"
#include "Vision/visionSettings.h"
#include "Visual/imageSettings.h"
#include <string>

struct appSettings
{
    std::string activeProfile = "assistant";
    llmSettings llm;
    intelligenceSettings intelligence;
    embeddingSettings embedding;
    speechSettings speech;
    speechRecognitionSettings speechRecognition;
    performanceSettings performance;
    improvementSettings improvement;
    presenceSettings presence;
    resourceSettings resources;
    visionSettings vision;
    perceptionSettings perception;
    initiativeSettings initiative;
    bargeInSettings bargeIn;
    conversationChannelSettings channels;
    conversationSettings conversation;
    responseFilterSettings responseFilter;
    imageSettings image;
    inputArbiterSettings inputArbiter;
    computerControlSettings computerControl;
};
