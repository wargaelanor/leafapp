package com.leafcontrol.app.net

import org.junit.Assert.assertEquals
import org.junit.Test

class ServerApiTest {
    @Test fun samplesUseSelectedDevice() {
        assertEquals("/api/samples?from=10&to=20&device=7", ServerApi.samplesPath(10, 20, 7))
        assertEquals("/api/samples?from=10&to=20", ServerApi.samplesPath(10, 20, 0))
    }

    @Test fun sampleAgeNeverReplacesKnownEpoch() {
        assertEquals(1_000_000L, ServerApi.sampleTimestampMs(1000, 900, 2_000_000))
        assertEquals(1_100_000L, ServerApi.sampleTimestampMs(0, 900, 2_000_000))
        assertEquals(0L, ServerApi.sampleTimestampMs(0, 9000, 2_000_000))
        assertEquals(0L, ServerApi.sampleTimestampMs(0, -1, 2_000_000))
    }
}
