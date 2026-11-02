package ai.zetic.lfm3demo

import android.content.res.AssetManager

/** One user turn of a sample scenario (samples/<name>/turns.txt, shared with the IQ9 CLI). */
class SampleTurn(val text: String, val imageFile: String?, val audioFile: String?, val out: String)

class Scenario(val name: String, val turns: List<SampleTurn>, val expected: String)

object Scenarios {
    /** Order shown in the Samples list. */
    val names = listOf("chat", "summary", "image", "image_count", "image_ocr", "asr", "tts", "vl_audio", "voice")
    val titles = mapOf(
        "chat" to "Chat: three-turn conversation", "summary" to "Summarize a long text (2k tokens)",
        "image" to "Image: describe the circles", "image_count" to "Image: count the circles", "image_ocr" to "Image: read a word",
        "asr" to "Audio: transcribe a speech clip", "tts" to "Text to speech (3 sentences)",
        "vl_audio" to "Image + spoken answer", "voice" to "Voice question -> spoken answer",
    )

    fun load(am: AssetManager, name: String): Scenario {
        fun read(p: String) = am.open(p).use { String(it.readBytes(), Charsets.UTF_8) }
        val expected = read("$name/expected.txt").trim()
        var out = "text"
        val turns = ArrayList<SampleTurn>()
        // blocks are separated by a line "---"; "out=" is read from the first block
        read("$name/turns.txt").split(Regex("(?m)^---\\s*$")).forEachIndexed { i, block ->
            var text = ""; var textFile: String? = null; var image: String? = null; var audio: String? = null
            for (line in block.lines()) {
                val k = line.substringBefore("=", ""); val v = line.substringAfter("=", "")
                when (k) {
                    "out" -> if (i == 0) out = v.trim()
                    "text" -> text = v
                    "text_file" -> textFile = v.trim()
                    "image" -> image = v.trim()
                    "audio" -> audio = v.trim()
                }
            }
            if (textFile != null) text = (if (text.isEmpty()) "" else text + "\n\n") + read("$name/$textFile").trim()
            turns.add(SampleTurn(text, image?.let { "$name/$it" }, audio?.let { "$name/$it" }, out))
        }
        return Scenario(name, turns.map { SampleTurn(it.text, it.imageFile?.let { f -> normalize(f) }, it.audioFile, out) }, expected)
    }

    // "vl_audio/../image/cats.ppm" -> "image/cats.ppm"
    private fun normalize(p: String): String {
        val parts = ArrayList<String>()
        for (s in p.split("/")) if (s == "..") parts.removeLastOrNull() else parts.add(s)
        return parts.joinToString("/")
    }
}
