# ResponseAudioTranscriptDoneEvent

Emitted when the full audio transcript is completed.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.audio.transcript.done&#x60;.  | 

## Example

```python
from openapi_client.models.response_audio_transcript_done_event import ResponseAudioTranscriptDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseAudioTranscriptDoneEvent from a JSON string
response_audio_transcript_done_event_instance = ResponseAudioTranscriptDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseAudioTranscriptDoneEvent.to_json())

# convert the object into a dict
response_audio_transcript_done_event_dict = response_audio_transcript_done_event_instance.to_dict()
# create an instance of ResponseAudioTranscriptDoneEvent from a dict
response_audio_transcript_done_event_from_dict = ResponseAudioTranscriptDoneEvent.from_dict(response_audio_transcript_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


