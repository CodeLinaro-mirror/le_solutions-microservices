# RealtimeSessionInputAudioNoiseReduction

Configuration for input audio noise reduction. This can be set to `null` to turn off. Noise reduction filters audio added to the input audio buffer before it is sent to VAD and the model. Filtering the audio can improve VAD and turn detection accuracy (reducing false positives) and model performance by improving perception of the input audio. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Type of noise reduction. &#x60;near_field&#x60; is for close-talking microphones such as headphones, &#x60;far_field&#x60; is for far-field microphones such as laptop or conference room microphones.  | [optional] 

## Example

```python
from openapi_client.models.realtime_session_input_audio_noise_reduction import RealtimeSessionInputAudioNoiseReduction

# TODO update the JSON string below
json = "{}"
# create an instance of RealtimeSessionInputAudioNoiseReduction from a JSON string
realtime_session_input_audio_noise_reduction_instance = RealtimeSessionInputAudioNoiseReduction.from_json(json)
# print the JSON string representation of the object
print(RealtimeSessionInputAudioNoiseReduction.to_json())

# convert the object into a dict
realtime_session_input_audio_noise_reduction_dict = realtime_session_input_audio_noise_reduction_instance.to_dict()
# create an instance of RealtimeSessionInputAudioNoiseReduction from a dict
realtime_session_input_audio_noise_reduction_from_dict = RealtimeSessionInputAudioNoiseReduction.from_dict(realtime_session_input_audio_noise_reduction_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


