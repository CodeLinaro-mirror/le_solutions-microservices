# ResponseAudioDoneEvent

Emitted when the audio response is complete.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.audio.done&#x60;.  | 

## Example

```python
from openapi_client.models.response_audio_done_event import ResponseAudioDoneEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseAudioDoneEvent from a JSON string
response_audio_done_event_instance = ResponseAudioDoneEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseAudioDoneEvent.to_json())

# convert the object into a dict
response_audio_done_event_dict = response_audio_done_event_instance.to_dict()
# create an instance of ResponseAudioDoneEvent from a dict
response_audio_done_event_from_dict = ResponseAudioDoneEvent.from_dict(response_audio_done_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


