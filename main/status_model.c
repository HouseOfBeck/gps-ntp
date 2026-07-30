#include "status_model.h"

#include <string.h>

#include "app_config.h"
#include "ethernet_w5500.h"
#include "esp_timer.h"
#include "ntp_server.h"

status_model_snapshot_t status_model_snapshot(void)
{
    clock_snapshot_t clock = clock_discipline_snapshot();
    ntp_server_status_t ntp = ntp_server_status();
    ethernet_status_t ethernet = ethernet_w5500_status();
    int64_t measured_rate = clock.esp_us_per_second_x1000;
    int64_t frequency_correction_milli_ppm =
        measured_rate > 0
            ? ((1000000000LL - measured_rate) * 1000000000LL) /
                  measured_rate
            : 0;
    status_model_snapshot_t status = {
        .clock_state = clock.state,
        .clock_synchronized = clock.synchronized,
        .pps_count = clock.pps_count,
        .last_pps_interval_us = clock.last_pps_interval_us,
        .frequency_correction_available = clock.rate_valid,
        .frequency_correction_milli_ppm =
            frequency_correction_milli_ppm,
        /*
         * The current discipline model estimates frequency but does not
         * calculate an independent phase-error metric.
         */
        .phase_error_available = false,
        .phase_error_us = 0,
        .ntp_request_count = ntp.request_count,
        .ntp_response_count = ntp.response_count,
        .ntp_last_receive_valid = ntp.last_receive_valid,
        .ntp_last_transmit_valid = ntp.last_transmit_valid,
        .ntp_last_transmit_synchronized =
            ntp.last_transmit_synchronized,
        .ntp_last_receive_to_transmit_valid =
            ntp.last_receive_to_transmit_valid,
        .ntp_last_receive_unix_us = ntp.last_receive_unix_us,
        .ntp_last_transmit_unix_us = ntp.last_transmit_unix_us,
        .ntp_last_receive_to_transmit_us =
            ntp.last_receive_to_transmit_us,
        .ethernet_started = ethernet.started,
        .ethernet_link_up = ethernet.link_up,
        .has_ipv4 = ethernet.has_ipv4,
        .uptime_us = esp_timer_get_time(),
    };

    memcpy(status.ipv4, ethernet.ipv4, sizeof(status.ipv4));
    strlcpy(status.hostname, app_config_active_hostname(),
            sizeof(status.hostname));
    return status;
}
