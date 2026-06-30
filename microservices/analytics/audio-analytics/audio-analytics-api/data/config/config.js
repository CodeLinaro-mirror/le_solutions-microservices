/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
'use strict';

// Get Environment Variables from env file if not production
if (process.env.NODE_ENV !== 'production') {
    // Env Vars are obtained from Kubernetes/other platform in production
    require('dotenv').config();
}

const config = {
    // Server Configuration
    apiPort: parseInt(process.env.API_PORT) || 8085,
    inactivityTimer: parseInt(process.env.INACTIVITY_TIMER) || 30000,

    // Redis Configuration
    redisHost: process.env.REDIS_HOST || 'redis',
    redisPort: parseInt(process.env.REDIS_PORT) || 6379,

    // Blackbox Mode Configuration
    blackboxContainer: process.env.BLACKBOX_CONTAINER === 'true',
    socketPath: process.env.SOCKET_PATH || '/tmp/audio-sockets/audio-analytics.sock',

    // ASR Channels
    asrTranscriptionIn: process.env.ASR_TRANSCRIPTION_IN || 'asr.transcription.in',
    asrTranscriptionOut: process.env.ASR_TRANSCRIPTION_OUT || 'asr.transcription.out',
    asrModels: process.env.ASR_MODELS || 'asr.models',
    
    // T2T Channels
    t2tTranslationIn: process.env.T2T_TRANSLATION_IN || 't2t.translation.in',
    t2tTranslationOut: process.env.T2T_TRANSLATION_OUT || 't2t.translation.out',
    t2tModels: process.env.T2T_MODELS || 't2t.models',
    
    // TTS Channels
    ttsTextIn: process.env.TTS_TEXT_IN || 'tts.text.in',
    ttsAudioOut: process.env.TTS_AUDIO_OUT || 'tts.audio.out',
    ttsModels: process.env.TTS_MODELS || 'tts.models',
    ttsDevices: process.env.TTS_DEVICES || 'tts.devices',

    // ASR Device Channel
    asrDevices: process.env.ASR_DEVICES || 'asr.devices',

    // KPIs Channel
    audioKPI: 'audio.kpi',

    // Upload Path
    uploadPath: process.env.uploadPath || '/app/uploads/',
    TESTING_ON: process.env.TESTING_ON,

    // Logging
    logLevel: parseInt(process.env.LOG_LEVEL) || 20  // 10=DEBUG, 20=INFO, 30=WARNING, 40=ERROR
};

module.exports = config;
