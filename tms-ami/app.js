const AppConfig = require('./config/app.js')
const fnAmiEventHandler = require('./event')

let { user, secret, host, port } = AppConfig.ami
let client = require('asterisk-manager')(port, host, user, secret, true)

client.keepConnected()
client.on('connect', () => console.log('tms-ami connected'))
client.on('managerevent', fnAmiEventHandler)
client.on('data', (chunk) => console.log('tms-ami data', chunk))
client.on('response', (response) => console.log('tms-ami response', response))
client.on('disconnect', () => console.log('disconnect'))
client.on('reconnection', () => console.log('reconnection'))
client.on('internalError', (error) => console.log(error))
client.action({
  Action: 'Ping',
})
