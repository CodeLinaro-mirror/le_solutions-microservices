# RealtimeTranscriptionSessionCreateResponse

A new Realtime transcription session configuration.  When a session is created on the server via REST API, the session object also contains an ephemeral key. Default TTL for keys is one minute. This  property is not present when a session is updated via the WebSocket API. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**client_secret** | [**RealtimeTranscriptionSessionCreateResponseClientSecret**](RealtimeTranscriptionSessionCreateResponseClientSecret.md) |  | 
**modalities** | **List[str]** | The set of modalities the model can respond with. To disable audio, set this to [\&quot;text\&quot;].  | [optional] 
**input_audio_format** | **str** | The format of input audio. Options are &#x60;pcm16&#x60;, &#x60;g711_ulaw&#x60;, or &#x60;g711_alaw&#x60;.  | [optional] 
**input_audio_transcription** | [**RealtimeTranscriptionSessionCreateResponseInputAudioTranscription**](RealtimeTranscriptionSessionCreateResponseInputAudioTranscription.md) |  | [optional] 
**turn_detection** | [**RealtimeSessionCreateResponseTurnDetection**](RealtimeSessionCreateResponseTurnDetection.md) |  | [optional] 

## Example

```python
from openapi_client.models.realtime_transcription_session_create_response import RealtimeTranscriptionSessionCreateResponse

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeTranscriptionSessionCreateResponse from a JSON string
realtime_transcription_session_create_response_instance = RealtimeTranscriptionSessionCreateResponse.from_json(json)
# print the JSON string representation of the object
print(RealtimeTranscriptionSessionCreateResponse.to_json())

# convert the object into a dict
realtime_transcription_session_create_response_dict = realtime_transcription_session_create_response_instance.to_dict()
# create an instance of RealtimeTranscriptionSessionCreateResponse from a dict
realtime_transcription_session_create_response_from_dict = RealtimeTranscriptionSessionCreateResponse.from_dict(realtime_transcription_session_create_response_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


