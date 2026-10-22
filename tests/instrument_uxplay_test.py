#!/usr/bin/env python3
"""Check the privacy-critical source transforms without downloading UxPlay."""
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('instrument_uxplay', ROOT / 'tools/instrument-uxplay.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InstrumentationTest(unittest.TestCase):
    def test_gate_is_unique_and_before_original_rejection(self):
        source = '#include "raop.h"\n' + module.GATE + '        logger_log(original);\n    }\n'
        output = module.instrument_raop(source)
        self.assertIn('direct_request_diag_log(method, url,', output)
        self.assertIn('direct_info_eligible(method, url, protocol, probe_type,', output)
        self.assertIn('raop_handler_info(conn, request, *response, &info_data, &info_length);', output)
        self.assertIn('http_response_init(*response, "HTTP/1.1", 200, "OK");', output)
        self.assertIn('http_response_finish(*response, info_data, info_length);', output)
        self.assertIn('direct_request_diag_log_result(method, url, probe_type, protocol,', output)
        self.assertLess(output.index('direct_info_eligible'), output.index('logger_log(original)'))
        self.assertLess(output.index('http_response_finish(*response, info_data, info_length)'),
                        output.index('logger_log(original)'))
        self.assertLess(output.index('direct_request_diag_log'), output.index('logger_log(original)'))
        self.assertNotIn('raop->hls_support = true', output)
        self.assertIn('httpd_set_connection_type(raop->httpd,conn,CONNECTION_TYPE_PTTH)', output)
        self.assertNotIn('CONNECTION_TYPE_EVENT', output)
        self.assertIn('!strcmp(url,"/info?txtAirPlay")', output)
        self.assertIn('!strcmp(url,"/fp-setup2")', output)
        with self.assertRaises(ValueError):
            module.instrument_raop(source + module.GATE)

    def test_body_is_bounded_before_reallocation(self):
        source = '    request->data = realloc(request->data, request->datalen + length);\n'
        output = module.instrument_http_request(source)
        self.assertLess(output.index('length > 33554432U'), output.index('realloc('))
        with self.assertRaises(ValueError):
            module.instrument_http_request('different upstream parser')

    def test_raw_http_logs_are_removed(self):
        source = '''                    http_request_is_reverse(connection->request);
            if (new_request) {
                int readstart = 0;
                while (readstart < 8) unsafe_busy_loop();
            }
            if (http_request_is_reverse(connection->request)) {
                if (recv_datalen && logger_debug) {
                    logger_log(raw_reverse_buffer);
                }
                if (recv_datalen == 0) {
            if (http_request_has_error(connection->request)) {
                logger_log(raw_parse_buffer);
            }
            /* If request is finished, process and deallocate */
                // Callback the received data to raop
                logger_log(raw_url);
                httpd->callbacks.conn_request(connection->user_data, connection->request, &response);
'''
        output = module.instrument_httpd(source)
        self.assertNotIn('raw_reverse_buffer', output)
        self.assertNotIn('raw_parse_buffer', output)
        self.assertNotIn('raw_url', output)
        self.assertIn('request data omitted', output)
        self.assertNotIn('while (readstart < 8)', output)
        self.assertNotIn('unsafe_busy_loop', output)
        self.assertIn('http_request_set_reverse(connection->request);', output)
        self.assertIn('recv_datalen = read_result;', output)
        with self.assertRaises(ValueError):
            module.instrument_httpd('changed upstream HTTP logs')


if __name__ == '__main__':
    unittest.main()
