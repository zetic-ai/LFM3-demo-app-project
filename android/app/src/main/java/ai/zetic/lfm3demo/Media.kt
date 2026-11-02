package ai.zetic.lfm3demo

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

/** Sample asset readers and the speech result player. */
object Media {
    /** Binary PPM (P6, maxval 255) -> RGB bytes; must be 512 x 512 for the SDK. */
    fun parsePpm(data: ByteArray): Triple<ByteArray, Int, Int> {
        var pos = 0
        fun token(): String {
            while (true) {
                while (data[pos].toInt().toChar().isWhitespace()) pos++
                if (data[pos].toInt().toChar() == '#') { while (data[pos].toInt().toChar() != '\n') pos++ } else break
            }
            val s = pos
            while (!data[pos].toInt().toChar().isWhitespace()) pos++
            return String(data, s, pos - s)
        }
        require(token() == "P6") { "not a P6 PPM" }
        val w = token().toInt(); val h = token().toInt(); require(token() == "255") { "maxval must be 255" }
        pos++
        return Triple(data.copyOfRange(pos, pos + w * h * 3), w, h)
    }

    class Pcm(val samples: FloatArray, val rate: Int, val channels: Int) {
        val seconds get() = samples.size.toDouble() / channels / rate
    }

    /** RIFF WAV reader: PCM 8/16/24/32 bit or 32-bit float, any rate and channel count. */
    fun readWav(data: ByteArray): Pcm {
        val bb = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        require(data.size > 44 && String(data, 0, 4) == "RIFF" && String(data, 8, 4) == "WAVE") { "not a WAV file" }
        var pos = 12; var fmt = 1; var ch = 1; var rate = 16000; var bits = 16
        while (pos + 8 <= data.size) {
            val id = String(data, pos, 4); val size = bb.getInt(pos + 4); val body = pos + 8
            require(size >= 0 && size.toLong() <= data.size - body) { "truncated WAV chunk" }
            if (id == "fmt ") { require(size >= 16) { "truncated WAV format" }; fmt = bb.getShort(body).toInt() and 0xFFFF; ch = bb.getShort(body + 2).toInt(); rate = bb.getInt(body + 4); bits = bb.getShort(body + 14).toInt()
                if (fmt == 0xFFFE) { require(size >= 26) { "truncated WAV extension" }; fmt = bb.getShort(body + 24).toInt() and 0xFFFF } }
            if (id == "data") {
                require(ch > 0 && rate > 0 && fmt in listOf(1, 3) && bits in listOf(8, 16, 24, 32) && (fmt != 3 || bits == 32)) { "unsupported WAV format" }
                val n = min(size.toLong() and 0xFFFFFFFFL, (data.size - body).toLong()).toInt() / (bits / 8)
                val out = FloatArray(n)
                for (i in 0 until n) out[i] = when {
                    fmt == 3 && bits == 32 -> bb.getFloat(body + i * 4)
                    bits == 16 -> bb.getShort(body + i * 2) / 32768f
                    bits == 8 -> ((data[body + i].toInt() and 0xFF) - 128) / 128f
                    bits == 24 -> (((data[body + i * 3 + 2].toInt() shl 16) or ((data[body + i * 3 + 1].toInt() and 0xFF) shl 8) or (data[body + i * 3].toInt() and 0xFF)) / 8388608f)
                    bits == 32 -> bb.getInt(body + i * 4) / 2147483648f
                    else -> throw IllegalArgumentException("unsupported WAV format $fmt/$bits")
                }
                return Pcm(out, rate, ch)
            }
            pos = body + size + (size and 1)
        }
        throw IllegalArgumentException("WAV has no data chunk")
    }

    fun writeWav(file: File, pcm: FloatArray, rate: Int) {
        val bb = ByteBuffer.allocate(44 + pcm.size * 2).order(ByteOrder.LITTLE_ENDIAN)
        bb.put("RIFF".toByteArray()).putInt(36 + pcm.size * 2).put("WAVEfmt ".toByteArray()).putInt(16).putShort(1).putShort(1)
            .putInt(rate).putInt(rate * 2).putShort(2).putShort(16).put("data".toByteArray()).putInt(pcm.size * 2)
        for (f in pcm) bb.putShort((f.coerceIn(-1f, 1f) * 32767f).toInt().toShort())
        file.parentFile?.mkdirs(); file.writeBytes(bb.array())
    }

}

/** Plays float PCM; chunks added with [write] start playing at once (low-latency streaming of the SDK's audio chunks). */
class StreamPlayer(private val rate: Int) {
    private val q = LinkedBlockingQueue<FloatArray>()
    private val stopped = AtomicBoolean(false)
    private val track = AudioTrack.Builder()
        .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
        .setAudioFormat(AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_FLOAT).setSampleRate(rate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
        .setBufferSizeInBytes(maxOf(AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT), rate * 4))
        .setTransferMode(AudioTrack.MODE_STREAM).build()
    private val worker = Thread {
        track.play()
        var written = 0L
        while (true) {
            val c = q.take()
            if (c.isEmpty() || stopped.get()) break
            var offset = 0
            while (offset < c.size && !stopped.get()) {
                val n = try { track.write(c, offset, c.size - offset, AudioTrack.WRITE_BLOCKING) }
                        catch (_: IllegalStateException) { break }
                if (n <= 0) break
                offset += n
                written += n
            }
        }
        // A blocking write only waits for buffer space. stop() before the playback head reaches
        // the last sample discards the queued tail (often the end of the spoken sentence).
        val deadline = android.os.SystemClock.uptimeMillis() + (written * 1000 / rate) + 2000
        while (!stopped.get() && (track.playbackHeadPosition.toLong() and 0xffffffffL) < written &&
            android.os.SystemClock.uptimeMillis() < deadline) {
            Thread.sleep(10)
        }
        android.util.Log.d("LFM3Demo", "PLAYBACK_${if (stopped.get()) "STOP" else "DRAIN"} written=$written played=${track.playbackHeadPosition.toLong() and 0xffffffffL}")
        try { track.stop() } catch (_: Exception) {}
        track.release()
    }.also { it.start() }

    fun write(chunk: FloatArray) { if (!stopped.get()) q.put(chunk.copyOf()) }
    fun finish() { q.put(FloatArray(0)) }
    fun stop() {
        if (!stopped.compareAndSet(false, true)) return
        try { track.pause(); track.flush(); track.stop() } catch (_: Exception) {}
        q.clear()
        q.offer(FloatArray(0))
    }
}
