function handler(amiEvent) {
  console.log('tms-ami event', amiEvent)
  if ('Cdr' === amiEvent.Event) {
    console.log('AMI-Event Cdr', amiEvent)
  } else if ('SessionTimeout' === amiEvent.Event) {
    console.log('AMI-Event SessionTimeout', amiEvent)
  } else if ('AgentRingNoAnswer' === amiEvent.Event) {
    console.log('AMI-Event AgentRingNoAnswer', amiEvent)
  }
}
module.exports = handler
