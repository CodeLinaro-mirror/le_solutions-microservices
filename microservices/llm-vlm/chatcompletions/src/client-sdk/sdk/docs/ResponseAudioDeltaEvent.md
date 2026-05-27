# ResponseAudioDeltaEvent

Emitted when there is a partial audio response.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;response.audio.delta&#x60;.  | 
**delta** | **str** | A chunk of Base64 encoded response audio bytes.  | 

## Example

```python
from openapi_client.models.response_audio_delta_event import ResponseAudioDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseAudioDeltaEvent from a JSON string
response_audio_delta_event_instance = ResponseAudioDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(ResponseAudioDeltaEvent.to_json())

# convert the object into a dict
response_audio_delta_event_dict = response_audio_delta_event_instance.to_dict()
# create an instance of ResponseAudioDeltaEvent from a dict
response_audio_delta_event_from_dict = ResponseAudioDeltaEvent.from_dict(response_audio_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


