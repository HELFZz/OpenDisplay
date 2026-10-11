package com.opendisplay.app

import android.content.Context
import android.content.SharedPreferences

object Settings {
    private const val PREFS = "opendisplay_settings"
    const val KEY_FPS = "fps"
    const val KEY_QUALITY = "quality"
    const val KEY_HOST = "host_ip"
    const val KEY_PORT = "port"

    // Quality presets: name -> QP value (lower = better quality)
    val QUALITY_PRESETS = linkedMapOf(
        "Ультра" to 12,
        "Высокое" to 18,
        "Среднее" to 22,
        "Низкое" to 28,
    )

    val FPS_OPTIONS = intArrayOf(30, 60, 90, 120)

    private fun prefs(ctx: Context): SharedPreferences =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun getFps(ctx: Context): Int = prefs(ctx).getInt(KEY_FPS, 120)
    fun setFps(ctx: Context, fps: Int) = prefs(ctx).edit().putInt(KEY_FPS, fps).apply()

    fun getQualityName(ctx: Context): String = prefs(ctx).getString(KEY_QUALITY, "Высокое") ?: "Высокое"
    fun setQualityName(ctx: Context, name: String) = prefs(ctx).edit().putString(KEY_QUALITY, name).apply()
    fun getQp(ctx: Context): Int = QUALITY_PRESETS[getQualityName(ctx)] ?: 18

    fun getHost(ctx: Context): String = prefs(ctx).getString(KEY_HOST, "127.0.0.1") ?: "127.0.0.1"
    fun setHost(ctx: Context, ip: String) = prefs(ctx).edit().putString(KEY_HOST, ip).apply()

    fun getPort(ctx: Context): Int = prefs(ctx).getInt(KEY_PORT, 27190)
    fun setPort(ctx: Context, port: Int) = prefs(ctx).edit().putInt(KEY_PORT, port).apply()
}
