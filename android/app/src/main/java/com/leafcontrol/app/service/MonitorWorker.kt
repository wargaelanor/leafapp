package com.leafcontrol.app.service

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import com.leafcontrol.app.data.Settings
import java.util.concurrent.TimeUnit

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent?) {
        if (intent?.action != Intent.ACTION_BOOT_COMPLETED) return
        if (!Settings.monitorEnabled) return
        try {
            val i = Intent(context, LeafMonitorService::class.java).setAction(LeafMonitorService.ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) context.startForegroundService(i)
            else context.startService(i)
        } catch (_: Exception) {
        }
    }
}

/** Restarts the foreground service if Android killed it. */
class MonitorWorker(context: Context, params: WorkerParameters) : Worker(context, params) {
    override fun doWork(): Result {
        if (!Settings.monitorEnabled) return Result.success()
        try {
            val i = Intent(applicationContext, LeafMonitorService::class.java)
                .setAction(LeafMonitorService.ACTION_START)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                applicationContext.startForegroundService(i)
            } else {
                applicationContext.startService(i)
            }
        } catch (_: Exception) {
        }
        return Result.success()
    }

    companion object {
        private const val WORK = "leaf-monitor-keepalive"

        fun schedule(context: Context) {
            val request = PeriodicWorkRequestBuilder<MonitorWorker>(15, TimeUnit.MINUTES).build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(
                WORK, ExistingPeriodicWorkPolicy.UPDATE, request
            )
        }
    }
}
