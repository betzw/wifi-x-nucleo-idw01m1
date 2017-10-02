/* SPWFSA04 Device
 * Copyright (c) 2015 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SPWFSA04.h"
#include "SpwfSAInterface.h"
#include "mbed_debug.h"

#if MBED_CONF_IDW0XX1_EXPANSION_BOARD == IDW04A1

SPWFSA04::SPWFSA04(PinName tx, PinName rx,
                   PinName rts, PinName cts,
                   SpwfSAInterface &ifce, bool debug,
                   PinName wakeup, PinName reset)
: SPWFSAxx(tx, rx, rts, cts, ifce, debug, wakeup, reset) {
}

bool SPWFSA04::open(const char *type, int* spwf_id, const char* addr, int port)
{
    int socket_id;
    int value;
    BH_HANDLER;

    if(!_parser.send("AT+S.SOCKON=%s,%d,NULL,%s", addr, port, type))
    {
        debug_if(true, "\r\nSPWF> error opening socket (%d)\r\n", __LINE__);
        return false;
    }

    /* handle both response possibilities here before returning
     * otherwise module seems to remain in inconsistent state.
     */

    if(!_parser.recv("AT-S.")) { // get prefix
        return false;
    }

    /* wait for next character */
    while((value = _parser.getc()) < 0);

    switch(value) {
        case 'O':
            /* get next character */
            value = _parser.getc();
            if(value != 'n') {
                debug_if(true, "\r\nSPWF> error opening socket (%d)\r\n", __LINE__);
                return false;
            }

            /* get socket id */
            if(!(_parser.recv(":%*u.%*u.%*u.%*u:%d%*[\x0d]", &socket_id)
                    && _recv_delim_lf()
                    && _recv_ok())) {
                debug_if(true, "\r\nSPWF> error opening socket (%d)\r\n", __LINE__);
                return false;
            }
            debug_if(true, "AT^ AT-S.On:%d\r\n", socket_id);

            *spwf_id = socket_id;
            return true;
        case 'E':
            int err_nr;
            if(_parser.recv("RROR:%d:%[^\x0d]%*[\x0d]", &err_nr, _err_msg_buffer) && _recv_delim_lf()) {
                debug_if(true, "AT^ AT-S.ERROR:%d:%s (%d)\r\n", err_nr, _err_msg_buffer, __LINE__);
            } else {
                debug_if(true, "\r\nSPWF> error opening socket (%d)\r\n", __LINE__);
            }
            break;
        default:
            debug_if(true, "\r\nSPWF> error opening socket (%d)\r\n", __LINE__);
            break;
    }

    return false;
}

bool SPWFSA04::_recv_ap(nsapi_wifi_ap_t *ap)
{
    bool ret;
    int curr;
    unsigned int channel;

    ap->security = NSAPI_SECURITY_UNKNOWN;

    /* determine list end */
    curr = _parser.getc();
    if(curr == 'A') { // assume end of list ("AT-S.OK")
        _parser.recv("T-S.OK%*[\x0d]") && _recv_delim_lf();
        return false;
    }

    /* run to 'horizontal tab' */
    while(_parser.getc() != '\x09');

    /* read in next line */
    ret = _parser.recv(" %*s %hhx:%hhx:%hhx:%hhx:%hhx:%hhx CHAN: %u RSSI: %hhd SSID: \'%256[^\']\' CAPS:",
                       &ap->bssid[0], &ap->bssid[1], &ap->bssid[2], &ap->bssid[3], &ap->bssid[4], &ap->bssid[5],
                       &channel, &ap->rssi, ssid_buf);

    if(ret) {
        int value;

        /* copy values */
        memcpy(&ap->ssid, ssid_buf, 32);
        ap->ssid[32] = '\0';
        ap->channel = channel;

        /* skip 'CAPS' */
        for(int i = 0; i < 6; i++) { // read next six characters (" 0421 ")
            _parser.getc();
        }

        /* get next character */
        value = _parser.getc();
        if(value != 'W') { // no security
            ap->security = NSAPI_SECURITY_NONE;
            goto recv_ap_get_out;
        }

        /* determine security */
        {
            char buffer[10];

            if(!_parser.recv("%s%*[\x20]", &buffer)) {
                goto recv_ap_get_out;
            } else if(strncmp("EP", buffer, 10) == 0) {
                ap->security = NSAPI_SECURITY_WEP;
                goto recv_ap_get_out;
            } else if(strncmp("PA2", buffer, 10) == 0) {
                ap->security = NSAPI_SECURITY_WPA2;
                goto recv_ap_get_out;
            } else if(strncmp("PA", buffer, 10) != 0) {
                goto recv_ap_get_out;
            }

            /* got a "WPA", check for "WPA2" */
            value = _parser.getc();
            if(value == _cr_) { // no further protocol
                ap->security = NSAPI_SECURITY_WPA;
                goto recv_ap_get_out;
            } else { // assume "WPA2"
                ap->security = NSAPI_SECURITY_WPA_WPA2;
                goto recv_ap_get_out;
            }
        }
    } else {
        debug("%s - ERROR: Should never happen!\r\n", __func__);
    }

recv_ap_get_out:
    if(ret) {
        /* wait for next line feed */
        while(!_recv_delim_lf());
    }

    return ret;
}

nsapi_size_or_error_t SPWFSA04::scan(WiFiAccessPoint *res, unsigned limit)
{
    unsigned int cnt = 0, found;
    nsapi_wifi_ap_t ap;

    if (!_parser.send("AT+S.SCAN=s,")) {
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    if(!(_parser.recv("AT-S.Parsing Networks:%u%*[\x0d]", &found) && _recv_delim_lf())) {
        debug_if(true, "SPWF> error start network scanning\r\n");
        return NSAPI_ERROR_DEVICE_ERROR;
    }

    debug_if(true, "AT^ AT-S.Parsing Networks:%u\r\n", found);

    if(found > 0) {
        while (_recv_ap(&ap)) {
            if (cnt < limit) {
                res[cnt] = WiFiAccessPoint(ap);
            }

            if (!((limit != 0) && ((cnt + 1) > limit))) {
                cnt++;
            }
        }
    } else {
        _recv_ok();
    }

    return cnt;
}

#endif // MBED_CONF_IDW0XX1_EXPANSION_BOARD
