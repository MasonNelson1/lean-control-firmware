const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const env = vm.createContext({fflate: require('../vendor/fflate-0.8.3.js'),
  TextEncoder, TextDecoder, Uint8Array, Uint32Array, DataView});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../profile-files.js'), 'utf8'), env);
const codec = vm.runInContext('LeanProfiles', env);
module.exports = {codec, env};
if (require.main === module) {
  const [, , command, input, output] = process.argv;
  const action = command === 'encode'
    ? codec.encode(JSON.parse(fs.readFileSync(input, 'utf8')))
    : codec.decode(fs.readFileSync(input));
  action.then(result => fs.writeFileSync(output, command === 'encode' ? result : JSON.stringify(result)))
    .catch(error => { console.error(error.message); process.exitCode = 1; });
}
