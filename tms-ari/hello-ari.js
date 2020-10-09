const client = require('ari-client')
const util = require('util')

function play(playback, channel, sound, callback) {
  playback.once('PlaybackFinished', function (event, instance) {
    console.log('PlaybackFinished...')
  })
  channel.play({ media: sound }, playback, function (err, playback) {
    if (callback) callback(err, playback, channel)
  })
}

function registerDtmfListeners(err, playback, incoming) {
  /* 收到按键 */
  incoming.on('ChannelDtmfReceived', (event, channel) => {
    var digit = event.digit
    console.log('tms-ari', 'ChannelDtmfReceived', digit)
    switch (digit) {
      case '5':
        playback.control({ operation: 'pause' }, function (err) {})
        break
      case '8':
        playback.control({ operation: 'unpause' }, function (err) {})
        break
      case '4':
        playback.control({ operation: 'reverse' }, function (err) {})
        break
      case '6':
        playback.control({ operation: 'forward' }, function (err) {})
        break
      case '2':
        playback.control({ operation: 'restart' }, function (err) {})
        break
      case '#':
        playback.stop(function (err) {
          play(playback, channel, 'sound:vm-goodbye', function (err) {
            channel.hangup(function (err) {
              //process.exit(0)
            })
          })
        })
        break
      case '*':
        play(playback, channel, 'sound:tt-monkeys', function (err) {})
        break
      default:
        play(playback, channel, util.format('sound:digits/%s', digit))
    }
  })
}

client
  .connect('http://192.168.43.165:8088', 'asterisk', 'asterisk')
  .then(function (ari) {
    console.log('tms-ari connected.')
    /* 进入应用 */
    ari.on('StasisStart', (event, incoming) => {
      /* 接听 */
      incoming.answer(function (err) {
        const playback = ari.Playback()

        registerDtmfListeners(err, playback, incoming)

        play(playback, incoming, 'sound:hello-world')
      })
    })
    /* 启动应用 */
    ari.start('hello-ari')
  })
  .catch(function (err) {})
