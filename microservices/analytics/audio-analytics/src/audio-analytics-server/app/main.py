# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Audio Analytics Server - Main Entry Point

This is the main orchestrator that initializes and manages all audio analytics services:
- ASR (Automatic Speech Recognition)
- T2T (Text-to-Text Translation)
- TTS (Text-to-Speech)

All services communicate via Redis pub/sub channels.
"""

import asyncio
import json
import signal
import sys
import time
from typing import List

import os
from common.config import Config
from common.logger import setup_logger, get_logger
from common.redis_client import create_redis_client, RedisClient
from common.socket_server import SocketServer
from common.communication_factory import CommunicationFactory
from services import ASRService, T2TService, TTSService
from common.base_service import BaseService

# Setup logging
setup_logger(__name__)
logger = get_logger(__name__)


class AudioAnalyticsServer:
    """
    Main server orchestrator for all audio analytics services.
    """

    def __init__(self):
        self.redis_client: RedisClient = None
        self.services: List[BaseService] = []
        self.running = False
        self._kpi_listener_task = None
        logger.info('=================================================================')
        logger.info('   NEW SESSION')
        logger.info('=================================================================')

    async def initialize(self):
        """Initialize communication client and all services."""
        logger.info('Initializing Audio Analytics Server...')

        # Determine if we're in blackbox mode
        blackbox_mode_str = os.environ.get('BLACKBOX_CONTAINER', 'false').lower()
        blackbox_mode = blackbox_mode_str in ('true', '1', 'yes')

        # Create appropriate communication client
        self.comm_client = await CommunicationFactory.create_client(blackbox_mode)

        # Initialize services
        logger.info('Initializing services...')
        self._asr = ASRService(self.comm_client)
        self._t2t = T2TService(self.comm_client)
        self._tts = TTSService(self.comm_client)
        self.services = [self._asr, self._t2t, self._tts]

        if not blackbox_mode:
            logger.info(f'Redis: {Config.REDIS_HOST}:{Config.REDIS_PORT}')

        logger.info(f'Initialized {len(self.services)} services')

    async def start(self):
        """Start all services."""
        logger.info('Starting Audio Analytics Server...')
        self.running = True

        # Start all services
        for service in self.services:
            await service.start()

        # Register KPI handler
        asr, t2t, tts = self._asr, self._t2t, self._tts

        _SHORT_TEXT = (
            "Whenever I find myself growing grim about the mouth, "
            "whenever it is a damp, drizzly November in my soul."
        )
        # Language-specific short texts for TTS benchmarking
        _TTS_SOURCE_TEXT = {
            'en': _SHORT_TEXT,
            'de': (
                "\u201eImmer wenn ich merke, dass sich um meinen Mund eine d\u00fcstere Stimmung breitmacht, "
                "immer wenn es in meiner Seele ein feuchter, nieseliger November ist.\u201c"
            ),
            'it': (
                "\u00abOgni volta che mi accorgo di avere un\u2019espressione cupa sul volto, "
                "ogni volta che nella mia anima \u00e8 un umido e piovigginoso novembre.\u00bb"
            ),
            'es': (
                "\u00abSiempre que me descubro ensombrecido de semblante, "
                "siempre que en mi alma reina un noviembre h\u00famedo y lluvioso.\u00bb"
            ),
            'zh': (
                "\u201c\u6bcf\u5f53\u6211\u53d1\u73b0\u81ea\u5df1\u6101\u5bb9\u6ee1\u9762\uff0c"
                "\u6bcf\u5f53\u6211\u7684\u7075\u9b42\u91cc\u6b63\u7ecf\u5386\u7740\u4e00\u4e2a"
                "\u6f6e\u6e7f\u3001\u9634\u90c1\u7684\u5341\u4e00\u6708\u65f6\u3002\u201d"
            ),
        }
        _LONG_TEXT = (
            "Whenever I find myself growing grim about the mouth, whenever it is a damp, drizzly November in my soul, "
            "whenever I find myself involuntarily pausing before coffin warehouses, and bringing up the rear of every "
            "funeral I meet, and especially whenever my hypos get such an upper hand of me that it require a strong "
            "moral principle to prevent me from deliberately stepping into the street, methodically knocking people's "
            "hats off, then I account it high time to get to the sea as soon as I can."
        )
        # Sample source text per language code for T2T benchmarking
        _T2T_SOURCE_TEXT = {
            'en': _SHORT_TEXT,
            'es': (
                "Siempre que me encuentro pensativo, "
                "cuando hay un noviembre frío y lluvioso en mi alma."
            ),
            'zh': (
                "每当我发现自己愁眉苦脸，"
                "每当我的心情像十一月的阴雨天般沉重。"
            ),
            'fr': (
                "Chaque fois que je me retrouve morose, "
                "chaque fois qu'il y a un novembre humide dans mon âme."
            ),
            'de': (
                "Immer wenn ich mich düster fühle, "
                "wenn es ein feuchter, trüber November in meiner Seele ist."
            ),
            'it': (
                "Ogni volta che mi accorgo di avere un'espressione cupa sul volto, "
                "ogni volta che nella mia anima è un umido e piovigginoso novembre."
            ),
        }

        def _do_kpi_benchmark():
            _t_kpi_start = time.time()
            result = {'tts': {}, 'asr': {}, 't2t': {}}

            _lang_name = {
                'en': 'English', 'es': 'Spanish', 'fr': 'French', 'de': 'German',
                'zh': 'Chinese', 'ja': 'Japanese', 'ko': 'Korean', 'ar': 'Arabic',
                'ru': 'Russian', 'pt': 'Portuguese', 'it': 'Italian'
            }

            # ── TTS benchmarks + ASR audio synthesis ───────────────────────────
            # For each TTS model:
            #   1. One instance, init_dir() — generates .qnn if missing, loads
            #      from cache if present. Wait for cache writer if running.
            #   2. benchmark() — C manages its own init/synth/deinit internally;
            #      the Python handle from init_dir() remains valid throughout.
            #   3. If this is the best English model seen so far (piper > melo >
            #      any English), synthesize short + long audio for ASR right now
            #      while the handle is still live — no re-init needed.
            #   4. deinit() and move to the next model.
            import ctypes as _ct

            short_audio = b""
            long_audio = b""
            synth_note = None
            _synth_priority = None   # tracks best English model found: 'piper' > 'melo' > 'other'

            _SYNTH_RANK = {"piper": 0, "melo": 1, "other": 2}

            if tts.tts_wrapper_class:
                for mc in tts.model_config:
                    name = mc.get("name", "unknown")
                    model_dir = mc.get("model_path", "")
                    if not model_dir:
                        result["tts"][name] = {"note": "model_path not configured"}
                        continue

                    print(f'[KPI] TTS {name}: ensuring cache...')
                    _tts_inst = tts.tts_wrapper_class()
                    try:
                        handle = _tts_inst.init_dir(model_dir, {})
                        # Wait for background cache writer if one is running
                        _cw = tts.tts_wrapper_class._cache_writer_thread
                        if _cw is not None and _cw.is_alive():
                            print(f'[KPI] TTS {name}: waiting for cache writer...')
                            _cw.join()
                        if not handle:
                            result["tts"][name] = {"note": "init_dir failed — model assets missing or invalid"}
                            continue
                    except Exception as e:
                        logger.error(f'TTS init_dir ({name}): {e}', exc_info=True)
                        result["tts"][name] = {"note": f"init_dir error: {e}"}
                        continue

                    print(f'[KPI] TTS {name}: benchmarking...')
                    try:
                        # Pick language-appropriate text for this model
                        _voice_lang = (mc.get("voices", [{}])[0].get("language", "en") or "en").lower()[:2]
                        _bench_text = _TTS_SOURCE_TEXT.get(_voice_lang, _SHORT_TEXT)
                        m = _tts_inst.benchmark(_bench_text, model_dir, {})
                        qnn_path = getattr(_tts_inst, '_resolved_qnn_path', None)
                        model_size_bytes = 0
                        if qnn_path and os.path.isfile(qnn_path):
                            model_size_bytes = os.path.getsize(qnn_path)
                        result["tts"][name] = {
                            "model_size_mb": model_size_bytes // (1024 * 1024),
                            "init_time_ms": m.get("init", 0),
                            "full_synthesis_latency_ms": m.get("total_latency", 0),
                            "time_to_first_chunk_ms": m.get("latency_first", 0),
                            "subsequent_chunk_latency_ms": m.get("latency_subsequent", 0),
                        }
                        print(f'[KPI] TTS {name}: total={m.get("total_latency", 0)}ms  first={m.get("latency_first", 0)}ms  subsequent={m.get("latency_subsequent", 0)}ms')
                    except Exception as e:
                        logger.error(f'TTS benchmark ({name}): {e}', exc_info=True)
                        result["tts"][name] = {"note": str(e)}

                    # Synthesize ASR audio if this is the best English model so far.
                    # benchmark() leaves the Python handle live, so process_with_callback
                    # works immediately — no re-init, no extra instance.
                    _is_english = any(
                        v.get("language", "").lower().startswith("en")
                        for v in mc.get("voices", [])
                    )
                    if _is_english and _synth_priority != "piper":
                        _rank = "piper" if name.startswith("piper") else ("melo" if name.startswith("melo") else "other")
                        if _synth_priority is None or _SYNTH_RANK[_rank] < _SYNTH_RANK[_synth_priority]:
                            print(f'[KPI] TTS {name}: synthesizing ASR audio (rank={_rank})...')
                            try:
                                _s_chunks = []
                                _l_chunks = []
                                def _cb_s(p, n): _s_chunks.append(_ct.string_at(p, n))
                                def _cb_l(p, n): _l_chunks.append(_ct.string_at(p, n))
                                _tts_inst.process_with_callback(_SHORT_TEXT, _cb_s)
                                short_audio = b"".join(_s_chunks)
                                _tts_inst.process_with_callback(_LONG_TEXT, _cb_l)
                                long_audio = b"".join(_l_chunks)
                                _synth_priority = _rank
                                print(f'[KPI] Short clip: {len(short_audio)} bytes, long clip: {len(long_audio)} bytes')
                            except Exception as e:
                                logger.error(f'TTS synthesize ASR audio ({name}): {e}', exc_info=True)
                                synth_note = f"TTS synthesis failed ({name}): {e}"

                    try:
                        _tts_inst.deinit()
                    except Exception:
                        pass

                if not tts.model_config:
                    synth_note = "no TTS models configured; ASR audio synthesis skipped"
                elif _synth_priority is None and not synth_note:
                    synth_note = "no English TTS model available; ASR audio synthesis skipped"
                if synth_note:
                    print(f'[KPI] ASR: {synth_note}')
            else:
                synth_note = "TTS engine not available; ASR audio synthesis skipped"
                print(f'[KPI] ASR: {synth_note}')

            # ── ASR benchmarks ──────────────────────────────────────────────────
            if asr.asr_engine:
                for mc in asr.model_config:
                    model_name = mc.get("name", "unknown")
                    model_dir = mc.get("model_path", "")
                    result["asr"][model_name] = {}

                    if synth_note:
                        result["asr"][model_name]["note"] = synth_note
                        continue
                    if not (short_audio or long_audio):
                        result["asr"][model_name]["note"] = "ASR audio synthesis produced no audio"
                        continue

                    assets = mc.get("assets", {})
                    path_key = "encoder_path" if assets.get("encoder_path") else "model_path"
                    encoder = os.path.join(model_dir, assets.get(path_key, "")).encode("utf-8")
                    decoder = os.path.join(model_dir, assets.get("decoder_path", "")).encode("utf-8")
                    vocab   = os.path.join(model_dir, assets.get("vocab_path", "")).encode("utf-8")
                    speech  = b"/usr/src/engine/models/whisper/speech_float.eai"
                    try:
                        whisper = asr.asr_engine.WhisperWrapper(language=None)
                        encoder_str = encoder.decode('utf-8')
                        decoder_str = decoder.decode('utf-8')
                        model_size_mb = (
                            (os.path.getsize(encoder_str) if os.path.isfile(encoder_str) else 0) +
                            (os.path.getsize(decoder_str) if os.path.isfile(decoder_str) else 0)
                        ) // (1024 * 1024)
                        result["asr"][model_name]["model_size_mb"] = model_size_mb
                        for clip_key, audio in [("short_clip", short_audio), ("long_clip", long_audio)]:
                            if not audio:
                                continue
                            print(f'[KPI] ASR {model_name} {clip_key}: benchmarking...')
                            try:
                                m = whisper.benchmark(audio, encoder, decoder, vocab, speech)
                                if clip_key == "short_clip":
                                    result["asr"][model_name]["init_time_ms"] = m.get("init", 0)
                                result["asr"][model_name][clip_key] = {
                                    "clip_bytes": len(audio),
                                    "token_count": m.get("tokens", 0),
                                    "time_to_first_token_ms": m.get("time_to_first_token", 0),
                                    "processing_latency_ms": m.get("total_latency", 0),
                                }
                                print(f'[KPI] ASR {model_name} {clip_key}: init={m.get("init", 0)}ms  proc={m.get("total_latency", 0)}ms  first_token={m.get("time_to_first_token", 0)}ms  tokens={m.get("tokens", 0)}')
                            except Exception as e:
                                logger.error(f'ASR benchmark ({model_name} {clip_key}): {e}', exc_info=True)
                                result["asr"][model_name][clip_key] = {"note": str(e)}
                    except Exception as e:
                        logger.error(f'ASR benchmark init ({model_name}): {e}', exc_info=True)
                        result["asr"][model_name]["note"] = str(e)

            # ── T2T benchmarks ──────────────────────────────────────────────────
            # Use the service's engine class directly. For each model:
            #   1. Create one TranslationWrapper — init_dir() resolves the cache
            #      (generates the .qnn if missing, reuses it if present).
            #   2. Wait for the background cache writer to finish.
            #   3. Call benchmark() with the resolved .qnn path.
            #   4. close() after benchmark so DSP resources are freed before
            #      the next model. T2T models each need exclusive DSP access.
            if t2t.t2t_engine:
                TranslationWrapper = t2t.t2t_engine.TranslationWrapper
                # Release the service's singleton so the KPI has exclusive DSP access.
                # The service will reinit on the next real translation request.
                if t2t.t2t_wrapper is not None:
                    print('[KPI] T2T: releasing service wrapper for exclusive DSP access...')
                    try:
                        t2t.t2t_wrapper.close()
                    except Exception as e:
                        logger.warning(f'[KPI] T2T: service wrapper close failed: {e}')
                    t2t.t2t_wrapper = None
                    t2t.current_wrapper_key = None

                for mc in t2t.model_config:
                    model_name = mc.get("name", "unknown")
                    model_dir = mc.get("model_path", "")
                    result["t2t"][model_name] = {}

                    src_code = mc.get("source_languages", [{}])[0].get("code", "en")
                    tgt_code = mc.get("target_languages", [{}])[0].get("code", "es")
                    src_text = _T2T_SOURCE_TEXT.get(src_code)
                    if not src_text:
                        result["t2t"][model_name]["note"] = (
                            f"skipped: no source-language text available for '{_lang_name.get(src_code, src_code)}'"
                        )
                        continue

                    input_lang  = _lang_name.get(src_code, src_code).encode("utf-8")
                    output_lang = _lang_name.get(tgt_code, tgt_code).encode("utf-8")

                    print(f'[KPI] T2T {model_name}: benchmarking...')
                    _t2t_wrapper = None
                    try:
                        _t2t_wrapper = TranslationWrapper(
                            model_path=None,
                            model_dir=model_dir,
                            input_lang=input_lang,
                            output_lang=output_lang
                        )
                        m = _t2t_wrapper.benchmark(src_text, model_dir, input_lang, output_lang)
                        result["t2t"][model_name] = {
                            "model_size_mb": m.get("model_size", 0) // (1024 * 1024),
                            "init_time_ms": m.get("init", 0),
                            "end_to_end_latency_ms": m.get("total_latency", 0),
                            "time_to_first_token_ms": m.get("time_to_first_token", 0),
                        }
                        print(f'[KPI] T2T {model_name}: init={m.get("init", 0)}ms  proc={m.get("total_latency", 0)}ms  first_token={m.get("time_to_first_token", 0)}ms')
                    except Exception as e:
                        logger.error(f'T2T benchmark ({model_name}): {e}', exc_info=True)
                        result["t2t"][model_name]["note"] = str(e)
                    finally:
                        if _t2t_wrapper:
                            try:
                                _t2t_wrapper.close()
                            except Exception:
                                pass

            _print_kpi_table(result)
            result["elapsed_s"] = round(time.time() - _t_kpi_start, 1)
            print(f'[KPI] Total elapsed: {result["elapsed_s"]}s')
            print('[KPI] ' + json.dumps(result, indent=2))
            return result

        def _print_kpi_table(result):
            W = 72
            print('[KPI] ' + '─' * W)
            print(f'[KPI] {"KPI BENCHMARK RESULTS":^{W}}')
            print('[KPI] ' + '─' * W)

            # TTS
            tts_r = result.get("tts", {})
            if tts_r:
                print(f'[KPI]  {"TTS":<16}  {"size_mb":>7}  {"init_ms":>7}  {"total_ms":>8}  {"first_ms":>8}  {"subseq_ms":>9}')
                print(f'[KPI]  {"─"*16}  {"─"*7}  {"─"*7}  {"─"*8}  {"─"*8}  {"─"*9}')
                for name, v in tts_r.items():
                    if "note" in v and "init_time_ms" not in v:
                        print(f'[KPI]  {name:<16}  (skipped: {v["note"]})')
                    else:
                        print(f'[KPI]  {name:<16}  {v.get("model_size_mb", 0):>7}  {v.get("init_time_ms", 0):>7}  {v.get("full_synthesis_latency_ms", 0):>8}  {v.get("time_to_first_chunk_ms", 0):>8}  {v.get("subsequent_chunk_latency_ms", 0):>9}')
                print('[KPI]')

            # ASR — iterate all models dynamically
            for model_name, asr_data in result.get("asr", {}).items():
                has_clips = asr_data.get("short_clip") or asr_data.get("long_clip")
                if not has_clips:
                    note = asr_data.get("note", "no data")
                    print(f'[KPI]  ASR ({model_name})  note: {note}')
                    print('[KPI]')
                    continue
                print(f'[KPI]  {"ASR (" + model_name + ")":<28}  init_ms={asr_data.get("init_time_ms", "–")}  size_mb={asr_data.get("model_size_mb", "–")}')
                if "note" in asr_data:
                    print(f'[KPI]    note: {asr_data["note"]}')
                print(f'[KPI]  {"clip":<10}  {"clip_bytes":>10}  {"tokens":>6}  {"proc_ms":>8}  {"first_token_ms":>14}')
                print(f'[KPI]  {"─"*10}  {"─"*10}  {"─"*6}  {"─"*8}  {"─"*14}')
                for clip_key in ("short_clip", "long_clip"):
                    c = asr_data.get(clip_key)
                    if not c:
                        continue
                    if "note" in c:
                        print(f'[KPI]  {clip_key:<10}  (error: {c["note"]})')
                    else:
                        print(f'[KPI]  {clip_key:<10}  {c.get("clip_bytes", 0):>10}  {c.get("token_count", 0):>6}  {c.get("processing_latency_ms", 0):>8}  {c.get("time_to_first_token_ms", 0):>14}')
                print('[KPI]')

            # T2T — iterate all models dynamically
            for model_name, t2t_data in result.get("t2t", {}).items():
                has_metrics = "init_time_ms" in t2t_data
                if not has_metrics:
                    note = t2t_data.get("note", "no data")
                    print(f'[KPI]  T2T ({model_name})  note: {note}')
                    print('[KPI]')
                    continue
                print(f'[KPI]  T2T ({model_name})  size_mb={t2t_data.get("model_size_mb", 0)}')
                if "note" in t2t_data:
                    print(f'[KPI]    note: {t2t_data["note"]}')
                print(f'[KPI]  {"init_ms":>8}  {"e2e_ms":>8}  {"first_token_ms":>15}')
                print(f'[KPI]  {"─"*8}  {"─"*8}  {"─"*15}')
                print(f'[KPI]  {t2t_data.get("init_time_ms", 0):>8}  {t2t_data.get("end_to_end_latency_ms", 0):>8}  {t2t_data.get("time_to_first_token_ms", 0):>15}')
                print('[KPI]')

            print('[KPI] ' + '─' * W)
            elapsed = result.get("elapsed_s")
            if elapsed is not None:
                print(f'[KPI]  Total elapsed: {elapsed}s')
                print('[KPI] ' + '─' * W)

        async def _run_kpi_benchmark(sync_id):
            try:
                print('[KPI] ── benchmark start ──────────────────────────')
                print(f'[KPI] Short text: "{_SHORT_TEXT}"')
                print(f'[KPI] Long text: "{_LONG_TEXT}"')
                result = await asyncio.to_thread(_do_kpi_benchmark)
                response = json.dumps({
                    'sync_id': sync_id,
                    'message_type': 'kpis_response',
                    'result': result,
                })
                await CommunicationFactory.publish(self.comm_client, Config.KPI_CHANNEL, response)
                logger.info('KPI benchmark complete')
            except Exception as e:
                logger.error(f'KPI benchmark error: {e}', exc_info=True)

        async def _run_kpi_last(sync_id):
            try:
                candidates = [
                    k for k in [asr._last_kpi, t2t._last_kpi, tts._last_kpi]
                    if k.get('ts') is not None
                ]
                result = max(candidates, key=lambda k: k['ts']) if candidates else {}
                response = json.dumps({
                    'sync_id': sync_id,
                    'message_type': 'kpis_response',
                    'result': result,
                })
                await CommunicationFactory.publish(self.comm_client, Config.KPI_CHANNEL, response)
                logger.info('KPI last complete')
            except Exception as e:
                logger.error(f'KPI last error: {e}', exc_info=True)

        def handle_kpi(message: str):
            try:
                msg = json.loads(message)
                sync_id = msg.get('sync_id')
                msg_type = msg.get('message_type')
                if msg_type == 'get_kpi_last':
                    asyncio.create_task(_run_kpi_last(sync_id))
                else:
                    asyncio.create_task(_run_kpi_benchmark(sync_id))
            except Exception as e:
                logger.error(f'KPI request error: {e}', exc_info=True)

        pubsub = await CommunicationFactory.subscribe(
            self.comm_client, {Config.KPI_CHANNEL: handle_kpi}
        )
        if pubsub:
            self._kpi_listener_task = asyncio.create_task(pubsub.run())

        logger.info('Audio Analytics Server started successfully')
        logger.info(f'Listening on channels:')
        logger.info(f'  ASR: {Config.ASR_TRANSCRIPTION_IN}, {Config.ASR_MODELS}')
        logger.info(f'  T2T: {Config.T2T_TRANSLATION_IN}, {Config.T2T_MODELS}')
        logger.info(f'  TTS: {Config.TTS_TEXT_IN}, {Config.TTS_MODELS}')
        logger.info(f'  KPI benchmark: {Config.KPI_CHANNEL} (get_kpis)')
        logger.info(f'  KPI last:      {Config.KPI_CHANNEL} (get_kpis_last)')

    async def stop(self):
        """Stop all services and cleanup."""
        logger.info('Stopping Audio Analytics Server...')
        self.running = False

        # Stop all services
        for service in self.services:
            try:
                await service.stop()
            except Exception as e:
                logger.error(f'Error stopping service: {e}')

        if self._kpi_listener_task:
            self._kpi_listener_task.cancel()
            try:
                await self._kpi_listener_task
            except asyncio.CancelledError:
                pass

        logger.info('Audio Analytics Server stopped')

    async def run(self):
        """Run the server until interrupted."""
        try:
            await self.initialize()
            await self.start()

            # Keep running until interrupted
            while self.running:
                await asyncio.sleep(1)

        except asyncio.CancelledError:
            logger.info('Server cancelled, shutting down...')
        except Exception as e:
            logger.error(f'Server error: {e}', exc_info=True)
        finally:
            await self.stop()


async def async_main():
    """Main async entry point."""
    server = AudioAnalyticsServer()

    # Setup signal handlers for graceful shutdown
    loop = asyncio.get_event_loop()

    def signal_handler():
        logger.info('Received shutdown signal')
        asyncio.create_task(server.stop())

    # Register signal handlers
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, signal_handler)

    # Run server
    await server.run()


def main():
    """Main entry point."""
    logger.info('='*60)
    logger.info('Audio Analytics Server')
    logger.info('='*60)
    logger.info(f'Log Level: {Config.LOG_LEVEL}')
    logger.info('='*60)

    try:
        asyncio.run(async_main())
    except KeyboardInterrupt:
        logger.info('Interrupted by user')
    except Exception as e:
        logger.error(f'Fatal error: {e}', exc_info=True)
        sys.exit(1)

    logger.info('Exiting Audio Analytics Server')


if __name__ == "__main__":
    main()
