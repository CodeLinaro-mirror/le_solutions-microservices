# ModelResponseProperties


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**metadata** | **Dict[str, str]** | Set of 16 key-value pairs that can be attached to an object. This can be useful for storing additional information about the object in a structured format, and querying for objects via API or the dashboard.   Keys are strings with a maximum length of 64 characters. Values are strings with a maximum length of 512 characters.  | [optional] 
**temperature** | **float** | What sampling temperature to use, between 0 and 2. Higher values like 0.8 will make the output more random, while lower values like 0.2 will make it more focused and deterministic. We generally recommend altering this or &#x60;top_p&#x60; but not both.  | [optional] [default to 1]
**top_p** | **float** | An alternative to sampling with temperature, called nucleus sampling, where the model considers the results of the tokens with top_p probability mass. So 0.1 means only the tokens comprising the top 10% probability mass are considered.  We generally recommend altering this or &#x60;temperature&#x60; but not both.  | [optional] [default to 1]
**user** | **str** | A unique identifier representing your end-user, which can help GenAI to monitor and detect abuse. [Learn more](/docs/guides/safety-best-practices#end-user-ids).  | [optional] 
**service_tier** | [**ServiceTier**](ServiceTier.md) |  | [optional] [default to ServiceTier.AUTO]

## Example

```python
from openapi_client.models.model_response_properties import ModelResponseProperties

# TODO update the JSON string below
json = "{}"
# create an instance of ModelResponseProperties from a JSON string
model_response_properties_instance = ModelResponseProperties.from_json(json)
# print the JSON string representation of the object
print(ModelResponseProperties.to_json())

# convert the object into a dict
model_response_properties_dict = model_response_properties_instance.to_dict()
# create an instance of ModelResponseProperties from a dict
model_response_properties_from_dict = ModelResponseProperties.from_dict(model_response_properties_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


