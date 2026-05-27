# CreateTranslationRequest


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**file** | **bytearray** | The audio file object (not file name) translate, in one of these formats: flac, mp3, mp4, mpeg, mpga, m4a, ogg, wav, or webm.  | 
**model** | [**CreateTranslationRequestModel**](CreateTranslationRequestModel.md) |  | 
**prompt** | **str** | An optional text to guide the model&#39;s style or continue a previous audio segment. The [prompt](/docs/guides/speech-to-text#prompting) should be in English.  | [optional] 
**response_format** | **str** | The format of the output, in one of these options: &#x60;json&#x60;, &#x60;text&#x60;, &#x60;srt&#x60;, &#x60;verbose_json&#x60;, or &#x60;vtt&#x60;.  | [optional] [default to 'json']
**temperature** | **float** | The sampling temperature, between 0 and 1. Higher values like 0.8 will make the output more random, while lower values like 0.2 will make it more focused and deterministic. If set to 0, the model will use log probability to automatically increase the temperature until certain thresholds are hit.  | [optional] [default to 0]

## Example

```python
from openapi_client.models.create_translation_request import CreateTranslationRequest

# TODO update the JSON string below
json = "{}"
# create an instance of CreateTranslationRequest from a JSON string
create_translation_request_instance = CreateTranslationRequest.from_json(json)
# print the JSON string representation of the object
print(CreateTranslationRequest.to_json())

# convert the object into a dict
create_translation_request_dict = create_translation_request_instance.to_dict()
# create an instance of CreateTranslationRequest from a dict
create_translation_request_from_dict = CreateTranslationRequest.from_dict(create_translation_request_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


