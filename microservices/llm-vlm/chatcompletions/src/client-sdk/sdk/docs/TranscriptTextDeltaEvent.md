# TranscriptTextDeltaEvent

Emitted when there is an additional text delta. This is also the first event emitted when the transcription starts. Only emitted when you [create a transcription](/docs/api-reference/audio/create-transcription) with the `Stream` parameter set to `true`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the event. Always &#x60;transcript.text.delta&#x60;.  | 
**delta** | **str** | The text delta that was additionally transcribed.  | 

## Example

```python
from openapi_client.models.transcript_text_delta_event import TranscriptTextDeltaEvent

# TODO update the JSON string below
json = "{}"
# create an instance of TranscriptTextDeltaEvent from a JSON string
transcript_text_delta_event_instance = TranscriptTextDeltaEvent.from_json(json)
# print the JSON string representation of the object
print(TranscriptTextDeltaEvent.to_json())

# convert the object into a dict
transcript_text_delta_event_dict = transcript_text_delta_event_instance.to_dict()
# create an instance of TranscriptTextDeltaEvent from a dict
transcript_text_delta_event_from_dict = TranscriptTextDeltaEvent.from_dict(transcript_text_delta_event_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


