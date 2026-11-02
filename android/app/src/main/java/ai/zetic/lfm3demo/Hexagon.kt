package ai.zetic.lfm3demo

import java.io.ByteArrayOutputStream

/** Thrown for every non-Ok SDK status. [status] is the SDK's name ("InvalidArgument", "ContextExceeded", "DeviceError", ...). */
class HexagonException(val status: String, val dspError: Int, message: String) :
    Exception("$status: $message" + if (dspError != 0) " (dsp error 0x${Integer.toHexString(dspError)})" else "")

internal fun errorFrom(s: String): HexagonException {
    val p = s.split("|", limit = 3)
    return HexagonException(p.getOrElse(0) { "Internal" }, p.getOrNull(1)?.toIntOrNull() ?: 0, p.getOrElse(2) { s })
}

/** LLMOptions. */
data class LlmOptions(
    val maxContext: Int = 0,             // 0 = package (2048); up to 4096 on phones
    val target: String = "",             // "" = detect; "v79" / "v81" forces one
    val verifyChecksums: Boolean = false,
    val warmup: Boolean = true,
    val hostAffinity: String = "",
    val maxNewTokens: Int = 512,
    val systemPrompt: String = "", val temperature: Float = -1f, val waitIfBusy: Boolean = true,
)

/** GenerationConfig + AudioOutputConfig + VideoConfig. -1 = the package's default. */
data class Generation(
    val maxTokens: Int = -1, val temperature: Float = -1f, val topP: Float = -1f, val minP: Float = -1f,
    val topK: Int = -1, val repetitionPenalty: Float = -1f, val repeatLastN: Int = 64, val seed: Long = 0,
    val enableThinking: Boolean = false,
    val textOut: Boolean = true, val audioOut: Boolean = false,           // modalities {Text}, {Audio}, {Text, Audio}
    val firstChunkFrames: Int = 1, val chunkFrames: Int = 12,             // audio chunking (80 ms per frame)
    val audioTemperature: Float = -1f, val audioTopK: Int = -1, val audioRepetitionPenalty: Float = -1f,
    val audioRepetitionWindow: Int = -1, val maxAudioSeconds: Float = 30f,
    val videoFps: Double = 2.0, val videoMinFrames: Int = 4, val videoMaxGroups: Int = 0,
    val cachePrompt: Boolean = true, val stop: List<String> = emptyList(),
    val logprobs: Boolean = false, val topLogprobs: Int = 0, val collectAudio: Boolean = true,
) {
    internal fun toArray() = doubleArrayOf(
        maxTokens.toDouble(), temperature.toDouble(), topP.toDouble(), minP.toDouble(), topK.toDouble(),
        repetitionPenalty.toDouble(), repeatLastN.toDouble(), seed.toDouble(), if (enableThinking) 1.0 else 0.0,
        ((if (textOut) 1 else 0) or (if (audioOut) 2 else 0)).toDouble(), firstChunkFrames.toDouble(), chunkFrames.toDouble(),
        audioTemperature.toDouble(), audioTopK.toDouble(), audioRepetitionPenalty.toDouble(), audioRepetitionWindow.toDouble(),
        maxAudioSeconds.toDouble(), videoFps, videoMinFrames.toDouble(), videoMaxGroups.toDouble(), if (cachePrompt) 1.0 else 0.0,
        if (logprobs) 1.0 else 0.0, topLogprobs.toDouble(), if (collectAudio) 1.0 else 0.0,
    )
}

/** ModelInfo + Capabilities. */
data class ModelInfo(val fields: Map<String, String>) {
    val modelId get() = fields["model_id"] ?: ""
    val soc get() = fields["soc"] ?: ""
    val arch get() = fields["arch"]?.toIntOrNull() ?: 0
    val image get() = fields["image"] == "1"
    val audio get() = fields["audio"] == "1"
    val video get() = fields["video"] == "1"
    val speechOut get() = fields["speech_out"] == "1"
    val maxContext get() = fields["max_context"]?.toIntOrNull() ?: 0
    override fun toString() = "$modelId on $soc (v$arch, context $maxContext)"
}

data class TokenLogprob(val token: Int, val piece: String, val logprob: Float, val top: List<Pair<Int, Float>>)

/** Response: text, the collected audio (24 kHz mono), Usage and Timings. */
data class Response(
    val text: String, val audio: FloatArray, val audioRate: Int, val finish: String,
    val ttftMs: Double, val tokensPerSecond: Double, val decodeSteps: Int,
    val firstAudioMs: Double, val audioSeconds: Double, val realtimeFactor: Double,
    val promptRows: Int, val cachedRows: Int, val completionTokens: Int, val audioFrames: Int,
    val prefillMs: Double, val encodeMs: Double,
    val tokens: List<Int>, val logprobs: List<TokenLogprob>, val stageTimings: List<Double>,
) {
    fun summary(): String {
        val sb = StringBuilder("TTFT %.0f ms".format(ttftMs))
        if (decodeSteps > 0) sb.append(", %.1f tokens/s".format(tokensPerSecond))
        if (audioSeconds > 0) sb.append(", first audio %.0f ms, %.1f s of speech (x%.2f real time)".format(firstAudioMs, audioSeconds, realtimeFactor))
        sb.append(", %d prompt rows (%d cached), %s".format(promptRows, cachedRows, finish))
        return sb.toString()
    }
}

/**
 * Clean Kotlin wrapper over the SDK's LLM class. One model per process; calls are serialized in native code.
 * Build a turn with the add* calls in prompt order (media first, then text, as the HF processors place them), then [chat].
 */
enum class MessageRole { SYSTEM, USER, ASSISTANT }

class HexagonLlm private constructor() : AutoCloseable {
    var info: ModelInfo = ModelInfo(emptyMap()); private set

    companion object {
        /** Slow (seconds): call on a background thread. [nativeLibDir] = context.applicationInfo.nativeLibraryDir. */
        fun load(modelDir: String, nativeLibDir: String, options: LlmOptions = LlmOptions()): HexagonLlm {
            val err = Native.load(modelDir, nativeLibDir, options.maxContext, options.target, options.verifyChecksums,
                options.warmup, options.hostAffinity, options.maxNewTokens, options.systemPrompt, options.temperature, options.waitIfBusy)
            if (err.isNotEmpty()) throw errorFrom(err)
            return HexagonLlm().also {
                it.info = ModelInfo(Native.info().lines().filter { l -> "=" in l }.associate { l -> l.substringBefore("=") to l.substringAfter("=") })
            }
        }
    }

    fun addSystem(text: String) = Native.addSystem(text)
    /** Commit pending parts as a prior system, user or assistant message. */
    fun appendMessage(role: MessageRole) { Native.appendMessage(role.ordinal).let { if (it.isNotEmpty()) throw errorFrom(it) } }
    fun addText(text: String) = Native.addText(text)
    /** rgb must be exactly 512 x 512 x 3 (the SDK never resizes). */
    fun addImage(rgb: ByteArray, w: Int = 512, h: Int = 512) { Native.addImage(rgb, w, h).let { if (it.isNotEmpty()) throw errorFrom(it) } }
    /** Float PCM at any rate (mono, or interleaved with [channels]); at most 30 s. */
    fun addAudio(pcm: FloatArray, sampleRate: Int, channels: Int = 1) { Native.addAudio(pcm, sampleRate, channels).let { if (it.isNotEmpty()) throw errorFrom(it) } }
    /** 512 x 512 RGB frames at [fps]. */
    fun addVideo(frames: List<ByteArray>, fps: Double = 2.0) { Native.addVideo(frames.toTypedArray(), 512, 512, fps).let { if (it.isNotEmpty()) throw errorFrom(it) } }

    fun tokenCount(text: String) = Native.tokenCount(text)
    fun tokenize(text: String, parseSpecial: Boolean = true) = Native.tokenize(text, parseSpecial)
    fun detokenize(tokens: IntArray) = Native.detokenize(tokens)
    fun reload() { Native.reload().let { if (it.isNotEmpty()) throw errorFrom(it) } }
    fun chatTemplate() = Native.chatTemplate()
    fun cancel() = Native.cancel()
    fun reset() = Native.reset()

    /**
     * Sends the turn. [onText] gets each streamed text piece, [onAudio] each speech chunk (24 kHz mono float) as it is
     * generated. Both run on the calling thread. Throws [HexagonException] for a non-Ok status (the turn is dropped).
     */
    fun chat(gen: Generation = Generation(), onText: (String) -> Unit = {}, onAudio: (FloatArray, Int) -> Unit = { _, _ -> }): Response {
        val text = StringBuilder()
        val audio = ArrayList<FloatArray>()
        var rate = 24000
        val pieces = Utf8Joiner()
        val res = Native.chat(gen.toArray(), gen.stop.toTypedArray(), object : NativeListener {
            override fun onText(utf8: ByteArray) { pieces.add(utf8).let { s -> if (s.isNotEmpty()) { text.append(s); onText(s) } } }
            override fun onAudio(pcm: FloatArray, sampleRate: Int) { rate = sampleRate; audio.add(pcm); onAudio(pcm, sampleRate) }
        })
        val p = res.split("|")
        if (p[0] != "Ok") throw errorFrom(res)
        val all = FloatArray(audio.sumOf { it.size }).also { var o = 0; for (a in audio) { a.copyInto(it, o); o += a.size } }
        fun d(i: Int) = p[i].toDoubleOrNull() ?: 0.0
        val tokens = p.getOrElse(14) { "" }.split(',').mapNotNull { it.toIntOrNull() }
        val probs = p.getOrElse(15) { "" }.split(';').filter { it.isNotEmpty() }.map { entry ->
            val fields = entry.split(',')
            val bytes = fields[2].chunked(2).map { it.toInt(16).toByte() }.toByteArray()
            TokenLogprob(fields[0].toInt(), String(bytes, Charsets.UTF_8), fields[1].toFloat(),
                fields.drop(3).map { it.substringBefore(':').toInt() to it.substringAfter(':').toFloat() })
        }
        return Response(text.toString(), all, rate, p[1], d(2), d(3), d(4).toInt(), d(5), d(6), d(7), d(8).toInt(), d(9).toInt(),
            d(10).toInt(), d(11).toInt(), d(12), d(13), tokens, probs, p.getOrElse(16) { "" }.split(',').mapNotNull { it.toDoubleOrNull() })
    }

    /** Destroys the LLM object, which frees the NPU. Loading again takes a few seconds. */
    override fun close() = Native.unload()
}

/** Joins byte pieces so a multi-byte UTF-8 character split across two pieces is not broken. */
class Utf8Joiner {
    private var carry = ByteArray(0)
    fun add(b: ByteArray): String {
        val all = carry + b
        var end = all.size
        var k = 1
        while (k <= 3 && k <= all.size) {
            val c = all[all.size - k].toInt() and 0xFF
            if (c and 0xC0 == 0x80) { k++; continue }
            val need = when { c >= 0xF0 -> 4; c >= 0xE0 -> 3; c >= 0xC0 -> 2; else -> 1 }
            if (need > k) end = all.size - k
            break
        }
        carry = all.copyOfRange(end, all.size)
        return String(all, 0, end, Charsets.UTF_8)
    }
}
