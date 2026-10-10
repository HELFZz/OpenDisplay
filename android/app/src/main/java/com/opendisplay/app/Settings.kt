package com.opendisplay.app

import android.content.Context
import android.content.SharedPreferences

object Settings {
    private const val PREFS = "opendisplay_settings"
    const val KEY_FPS = "fps"
    const val KEY_QP = "qp"
    const val KEY_HOST = "host_ip"
    const val KEY_PORT = "port"

    private fun prefs(ctx: Context): SharedPreferences =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun getFps(ctx: Context): Int = prefs(ctx).getInt(KEY_FPS, 120)
    fun setFps(ctx: Context, fps: Int) = prefs(ctx).edit().putInt(KEY_FPS, fps).apply()

    fun getQp(ctx: Context): Int = prefs(ctx).getInt(KEY_QP, 12)
    fun setQp(ctx: Context, qp: Int) = prefs(ctx).edit().putInt(KEY_QP, qp).apply()

    fun getHost(ctx: Context): String = prefs(ctx).getString(KEY_HOST, "127.0.0.1") ?: "127.0.0.1"
    fun setHost(ctx: Context, ip: String) = prefs(ctx).edit().putString(KEY_HOST, ip).apply()

    fun getPort(ctx: Context): Int = prefs(ctx).getInt(KEY_PORT, 27190)
    fun setPort(ctx: Context, port: Int) = prefs(ctx).edit().putInt(KEY_PORT, port).apply()
}
