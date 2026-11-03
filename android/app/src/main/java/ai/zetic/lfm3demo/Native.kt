package ai.zetic.lfm3demo

/** Raw JNI entry points (lfm3_jni.cpp). Use [HexagonLlm] instead. */
object Native {
    init { System.loadLibrary("lfm3demo") }
    @JvmStatic external fun load(modelDir: String, nativeLibDir: String, maxContext: Int, target: String, verify: Boolean,
                                 warmup: Boolean, hostAffinity: String, maxNew: Int, systemPrompt: String, temperature: Float, waitIfBusy: Boolean): String
    @JvmStatic external fun info(): String
    @JvmStatic external fun unload()
    @JvmStatic external fun reset()
    @JvmStatic external fun cancel()
    @JvmStatic external fun addSystem(text: String)
    @JvmStatic external fun appendMessage(role: Int): String
    @JvmStatic external fun addText(text: String)
    @JvmStatic external fun addImage(rgb: ByteArray, w: Int, h: Int): String
    @JvmStatic external fun addAudio(pcm: FloatArray, rate: Int, channels: Int): String
    @JvmStatic external fun addVideo(frames: Array<ByteArray>, w: Int, h: Int, fps: Double): String
    @JvmStatic external fun tokenCount(text: String): Int
    @JvmStatic external fun tokenize(text: String, parseSpecial: Boolean): IntArray
    @JvmStatic external fun detokenize(tokens: IntArray): String
    @JvmStatic external fun reload(): String
    @JvmStatic external fun chatTemplate(): String
    @JvmStatic external fun chat(cfg: DoubleArray, stop: Array<String>?, listener: NativeListener): String
    @JvmStatic external fun decision(modelDir: String, nativeLibDir: String, kind: String, state: String,
                                     pcm: FloatArray?, sampleRate: Int, channels: Int,
                                     rgb: ByteArray?, width: Int, height: Int): String
    @JvmStatic external fun decisionLoad(modelDir: String, nativeLibDir: String): String
    @JvmStatic external fun decisionUnload()
    @JvmStatic external fun decisionCancel()
}

interface NativeListener {
    fun onText(utf8: ByteArray)
    fun onAudio(pcm: FloatArray, sampleRate: Int)
}
