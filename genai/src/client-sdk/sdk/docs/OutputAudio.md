# OutputAudio

An audio output from the model. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the output audio. Always &#x60;output_audio&#x60;.  | 
**data** | **str** | Base64-encoded audio data from the model.  | 
**transcript** | **str** | The transcript of the audio data from the model.  | 

## Example

```python
from openapi_client.models.output_audio import OutputAudio

# TODO update the JSON string below
json = "{}"
# create an instance of OutputAudio from a JSON string
output_audio_instance = OutputAudio.from_json(json)
# print the JSON string representation of the object
print(OutputAudio.to_json())

# convert the object into a dict
output_audio_dict = output_audio_instance.to_dict()
# create an instance of OutputAudio from a dict
output_audio_from_dict = OutputAudio.from_dict(output_audio_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


