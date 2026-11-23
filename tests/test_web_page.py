from html.parser import HTMLParser
import json
import subprocess
import unittest

from host_harness import source


class Elements(HTMLParser):
    def __init__(self, html):
        super().__init__()
        self.elements = {}
        self.feed(html)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.elements[attrs["id"]] = {
                "value": attrs.get("value", ""),
                "hidden": "hidden" in attrs,
            }


def run_page(body, authorized=False):
    html = source("main/web/index.html")
    script = html.split("<script>", 1)[1].split("</script>", 1)[0]
    code = (
        "const elements = " + json.dumps(Elements(html).elements) + ";\n"
        "let authorized = " + json.dumps(authorized) + ";\n"
    ) + r"""
const assert = require('node:assert/strict');
const vm = require('node:vm');
const calls = [], downloads = [], intervals = [];
let joinState = 'idle', loginError = false;
for (const element of Object.values(elements)) {
  Object.assign(element, {
    style: {}, classList: {toggle(){}}, focus(){},
    textContent: '', innerHTML: '', className: '', disabled: false
  });
}
function response(status, data) {
  return {ok: status === 200, status, json: async()=>data, blob: async()=>({backup:true})};
}
const page = vm.createContext({
  document: {
    getElementById(id) { assert.ok(elements[id], 'missing element ' + id); return elements[id]; },
    createElement(tag) { assert.equal(tag, 'a'); return {click(){ downloads.push(this); }}; }
  },
  location: {port: '8573'},
  URL: {createObjectURL(blob){assert.ok(blob.backup); return 'blob:backup';}, revokeObjectURL(){}},
  confirm: ()=>true,
  alert: text=>{throw new Error(text);},
  setInterval: fn=>intervals.push(fn),
  setTimeout(){}, clearTimeout(){},
  async fetch(path, options) {
    calls.push({path, ...options});
    if (path === '/api/auth') {
      if (loginError) return response(429, {error:'Wait 30 seconds and try again.'});
      assert.equal(JSON.parse(options.body).code, '89AB');
      authorized = true;
      return response(200, {authenticated:true});
    }
    if (!authorized) return response(401, {error:'Enter the current access code.'});
    if (path === '/api/status') return response(200, {
      wifi: {connected:true, ssid:'LAN', ip:'192.0.2.1'},
      account: {callsign:'N0CALL', has_password:true, name:'Local', location:''},
      dir: {logged_in:true, count:1}, servers:0, favorites:0,
      ap: {active:false}, join: {state:joinState}
    });
    if (path.startsWith('/api/servers?')) return response(200, []);
    if (path === '/api/servers/export') return response(200, {});
    if (path === '/api/servers/import') return response(200, {settings:true, count:1});
    if (path === '/api/wifi') { joinState = 'ok'; return response(200, {}); }
    throw new Error('unexpected request: ' + path);
  }
});
const flush = ()=>new Promise(setImmediate);
"""
    code += "vm.runInContext(" + json.dumps(script) + ", page);\n"
    code += "(async()=>{await flush();\n" + body + "\n})().catch(e=>{console.error(e);process.exitCode=1;});\n"
    result = subprocess.run(["node", "-e", code], text=True, capture_output=True, timeout=10)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)


class WebPageTests(unittest.TestCase):
    def test_login_errors_restart_expiry_and_cardputer_reconnect_port(self):
        run_page(r"""
assert.equal(elements.config.hidden, true);
assert.equal(elements['s-auth'].hidden, false);
elements.code.value = '89AB';
loginError = true;
await elements['auth-form'].onsubmit({preventDefault(){}});
assert.equal(elements.config.hidden, true);
assert.match(elements['auth-msg'].textContent, /Wait 30 seconds/);
assert.equal(elements.unlock.disabled, false);
loginError = false;
await elements['auth-form'].onsubmit({preventDefault(){}});
await flush();
assert.equal(elements.config.hidden, false);
assert.equal(elements['s-auth'].hidden, true);
assert.equal(elements.code.value, '');
await elements.join.onclick();
await page.refresh();
assert.match(elements.wmsg.textContent, /http:\/\/192\.0\.2\.1:8573/);
assert.ok(calls.every(call=>call.credentials === 'same-origin'));
authorized = false;
intervals[0]();
await flush();
assert.equal(elements.config.hidden, true);
assert.equal(elements['s-auth'].hidden, false);
""")

    def test_backups_use_authorized_fetch_and_expired_sessions_cannot_download(self):
        run_page(r"""
assert.equal(elements.config.hidden, false);
await elements.dl.onclick();
assert.equal(downloads.length, 1);
assert.equal(downloads[0].href, 'blob:backup');
assert.equal(downloads[0].download, 'scheff-echolink-backup.xml');
await elements.upfile.onchange({target:{
  files:[{name:'backup.xml', text:async()=>'<echolink-backup/>'}], value:'backup.xml'
}});
await flush();
const upload = calls.find(call=>call.path === '/api/servers/import');
assert.equal(upload.method, 'POST');
assert.equal(upload.headers['Content-Type'], 'application/xml');
assert.equal(upload.body, '<echolink-backup/>');
assert.equal(upload.credentials, 'same-origin');
assert.match(elements.bmsg.textContent, /Restored all settings and 1 servers/);
authorized = false;
await elements.dl.onclick();
assert.equal(downloads.length, 1);
assert.equal(elements.config.hidden, true);
assert.match(elements.bmsg.textContent, /current access code/);
""", authorized=True)


if __name__ == "__main__":
    unittest.main()
