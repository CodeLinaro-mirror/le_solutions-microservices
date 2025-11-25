# RealtimeTranscriptionSessionCreateRequest

Realtime transcription session object configuration.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**modalities** | **List[str]** | The set of modalities the model can respond with. To disable audio, set this to [\&quot;text\&quot;].  | [optional] 
**input_audio_format** | **str** | The format of input audio. Options are &#x60;pcm16&#x60;, &#x60;g711_ulaw&#x60;, or &#x60;g711_alaw&#x60;. For &#x60;pcm16&#x60;, input audio must be 16-bit PCM at a 24kHz sample rate,  single channel (mono), and little-endian byte order.  | [optional] [default to 'pcm16']
**input_audio_transcription** | [**RealtimeTranscriptionSessionCreateRequestInputAudioTranscription**](RealtimeTranscriptionSessionCreateRequestInputAudioTranscription.md) |  | [optional] 
**turn_detection** | [**RealtimeTranscriptionSessionCreateRequestTurnDetection**](RealtimeTranscriptionSessionCreateRequestTurnDetection.md) |  | [optional] 
**input_audio_noise_reduction** | [**RealtimeSessionInputAudioNoiseReduction**](RealtimeSessionInputAudioNoiseReduction.md) |  | [optional] 
**include** | **List[str]** | The set of items to include in the transcription. Current available items are: - &#x60;item.input_audio_transcription.logprobs&#x60;  | [optional] 

## Example

```python
from openapi_client.models.realtime_transcription_session_create_request import RealtimeTranscriptionSessionCreateRequest

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeTranscriptionSessionCreateRequest from a JSON string
realtime_transcription_session_create_request_instance = RealtimeTranscriptionSessionCreateRequest.from_json(json)
# print the JSON string representation of the object
print(RealtimeTranscriptionSessionCreateRequest.to_json())

# convert the object into a dict
realtime_transcription_session_create_request_dict = realtime_transcription_session_create_request_instance.to_dict()
# create an instance of RealtimeTranscriptionSessionCreateRequest from a dict
realtime_transcription_session_create_request_from_dict = RealtimeTranscriptionSessionCreateRequest.from_dict(realtime_transcription_session_create_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


