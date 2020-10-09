/**
 *  This example shows how a call can be originated from a channel entering a
 *  Stasis application to an endpoint. The endpoint channel will then enter the
 *  Stasis application and the 2 channels will be placed into a mixing bridge.
 *
 *  @namespace originate-example
 *
 */

'use strict'

const client = require('ari-client')

//var ENDPOINT = 'PJSIP/8000'
const ENDPOINT = 'Local/3020@from-internal'
const ENDPOINT2 = 'Local/3012@from-internal'

// replace ari.js with your Asterisk instance
client.connect(
  'http://192.168.43.165:8088',
  'asterisk',
  'asterisk',
  /**
   *  Setup event listeners and start application.
   *
   *  @callback connectCallback
   *  @memberof originate-example
   *  @param {Error} err - error object if any, null otherwise
   *  @param {module:ari-client~Client} ari - ARI client
   */
  function (err, ari) {
    // Use once to start the application to ensure this listener will only run
    // for the incoming channel
    ari.on(
      'StasisStart',
      /**
       *  Once the incoming channel has entered Stasis, answer it and originate
       *  call to the endpoint (outgoing channel).
       *
       *  @callback incomingStasisStartCallback
       *  @memberof originate-example
       *  @param {Object} event - the full event object
       *  @param {module:resources~Channel} incoming -
       *    the incoming channel entering Stasis
       */
      function (event, incoming) {
        console.log('StasisStart', event.args)
        if (
          Array.isArray(event.args) &&
          event.args.length > 0 &&
          event.args[0] === 'dialplan'
        ) {
          incoming.answer(function (err) {
            originate(incoming)
          })
        }
      }
    )

    /**
     *  Originate the outgoing channel
     *
     *  @function originate
     *  @memberof originate-example
     *  @param {module:resources~Channel} incoming - the incoming channel that
     *    will originate the call to the endpoint
     */
    function originate(incoming) {
      incoming.once(
        'StasisEnd',
        /**
         *  If the incoming channel ends, hangup the outgoing channel.
         *
         *  @callback incomingStasisEndCallback
         *  @memberof originate-example
         *  @param {Object} event - the full event object
         *  @param {module:resources~Channel} channel -
         *    the incoming channel leaving Stasis
         */
        function (event, channel) {
          console.log('incoming StasisEnd')
          outgoing.hangup(function (err) {})
        }
      )

      let bridge = ari.Bridge()
      let outgoing = ari.Channel()
      let outgoing2 = ari.Channel()

      outgoing.once(
        'ChannelDestroyed',
        /**
         *  If the endpoint rejects the call, hangup the incoming channel.
         *
         *  @callback outgoingChannelDestroyedCallback
         *  @memberof originate-example
         *  @param {Object} event - the full event object
         *  @param {module:resources~Channel} channel -
         *    the channel that was destroyed
         */
        function (event, channel) {
          console.log('outgoing ChannelDestroyed')
          incoming.hangup(function (err) {})
        }
      )
      outgoing2.once(
        'ChannelDestroyed',
        /**
         *  If the endpoint rejects the call, hangup the incoming channel.
         *
         *  @callback outgoingChannelDestroyedCallback
         *  @memberof originate-example
         *  @param {Object} event - the full event object
         *  @param {module:resources~Channel} channel -
         *    the channel that was destroyed
         */
        function (event, channel) {
          console.log('outgoing2 ChannelDestroyed')
          incoming.hangup(function (err) {})
        }
      )

      outgoing.once(
        'StasisStart',
        /**
         *  When the outgoing channel enters Stasis, create a mixing bridge
         *  and join the channels together.
         *
         *  @callback outgoingStasisStartCallback
         *  @memberof originate-example
         *  @param {Object} event - the full event object
         *  @param {module:resources~Channel} outgoing -
         *    the outgoing channel entering Stasis
         */
        function (event, outgoing) {
          outgoing.once(
            'StasisEnd',
            /**
             *  If the outgoing channel ends, clean up the bridge.
             *
             *  @callback outgoingStasisEndCallback
             *  @memberof originate-example
             *  @param {Object} event - the full event object
             *  @param {module:resources~Channel} channel -
             *    the outgoing channel leaving Stasis
             */
            function (event, channel) {
              console.log('outgoing StasisEnd')
              bridge.destroy(function (err) {})
            }
          )

          outgoing.answer(
            /**
             *  Once the outgoing channel has been answered, create a mixing
             *  bridge and add the incoming and outgoing channels to it.
             *
             *  @callback outgoingAnswerCallback
             *  @memberof originate-example
             *  @param {Error} err - error object if any, null otherwise
             */
            function (err) {
              bridge.create(
                { type: 'mixing' },
                /**
                 *  Once the mixing bridge has been created, join the incoming and
                 *  outgoing channels to it.
                 *
                 *  @callback bridgeCreateCallback
                 *  @memberof originate-example
                 *  @param {Error} err - error object if any, null otherwise
                 *  @param {module:resources~Bridge} bridge - the newly created
                 *    mixing bridge
                 */
                function (err, bridge) {
                  bridge.addChannel(
                    { channel: [incoming.id, outgoing.id] },
                    function (err) {}
                  )
                }
              )
            }
          )
        }
      )
      outgoing2.once(
        'StasisStart',
        /**
         *  When the outgoing channel enters Stasis, create a mixing bridge
         *  and join the channels together.
         *
         *  @callback outgoingStasisStartCallback
         *  @memberof originate-example
         *  @param {Object} event - the full event object
         *  @param {module:resources~Channel} outgoing -
         *    the outgoing channel entering Stasis
         */
        function (event, outgoing) {
          outgoing.once(
            'StasisEnd',
            /**
             *  If the outgoing channel ends, clean up the bridge.
             *
             *  @callback outgoingStasisEndCallback
             *  @memberof originate-example
             *  @param {Object} event - the full event object
             *  @param {module:resources~Channel} channel -
             *    the outgoing channel leaving Stasis
             */
            function (event, channel) {
              console.log('outgoing2 StasisEnd')
              bridge.destroy(function (err) {})
            }
          )

          outgoing.answer(
            /**
             *  Once the outgoing channel has been answered, create a mixing
             *  bridge and add the incoming and outgoing channels to it.
             *
             *  @callback outgoingAnswerCallback
             *  @memberof originate-example
             *  @param {Error} err - error object if any, null otherwise
             */
            function (err) {
              bridge.addChannel({ channel: [outgoing2.id] }, function (err) {})
            }
          )
        }
      )

      let playback = ari.Playback()
      incoming.play({ media: 'sound:vm-dialout' }, playback, function (err) {})

      incoming.on('ChannelDtmfReceived', (event, channel) => {
        let digit = event.digit
        console.log('originate-example', 'ChannelDtmfReceived', digit)
        if (digit === '1') {
          ari.bridges.removeChannel({
            bridgeId: bridge.id,
            channel: outgoing.id,
          })
          outgoing2.originate(
            {
              endpoint: `${ENDPOINT}/n`,
              app: 'originate-example',
              appArgs: 'inapp2',
            },
            function (err, channel) {
              console.log('outgoing2 originate')
            }
          )
        }
      })

      // Originate call from incoming channel to endpoint
      outgoing.originate(
        {
          endpoint: `${ENDPOINT2}/n`,
          app: 'originate-example',
          appArgs: 'inapp',
        },
        function (err, channel) {
          console.log('outgoing originate')
        }
      )
    }

    // can also use ari.start(['app-name'...]) to start multiple applications
    ari.start('originate-example')
  }
)
