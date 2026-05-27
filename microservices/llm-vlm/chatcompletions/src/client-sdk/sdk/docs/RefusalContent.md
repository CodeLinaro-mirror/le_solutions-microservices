# RefusalContent

A refusal from the model.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the refusal. Always &#x60;refusal&#x60;. | [default to 'refusal']
**refusal** | **str** | The refusal explanationfrom the model. | 

## Example

```python
from openapi_client.models.refusal_content import RefusalContent

# TODO update the JSON string below
json = "{}"
# create an instance of RefusalContent from a JSON string
refusal_content_instance = RefusalContent.from_json(json)
# print the JSON string representation of the object
print(RefusalContent.to_json())

# convert the object into a dict
refusal_content_dict = refusal_content_instance.to_dict()
# create an instance of RefusalContent from a dict
refusal_content_from_dict = RefusalContent.from_dict(refusal_content_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


