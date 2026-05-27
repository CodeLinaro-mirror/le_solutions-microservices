# Reasoning

**o-series models only**  Configuration options for  [reasoning models]. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**effort** | [**ReasoningEffort**](ReasoningEffort.md) |  | [optional] [default to ReasoningEffort.MEDIUM]
**summary** | **str** | A summary of the reasoning performed by the model. This can be useful for debugging and understanding the model&#39;s reasoning process. One of &#x60;auto&#x60;, &#x60;concise&#x60;, or &#x60;detailed&#x60;.  | [optional] 
**generate_summary** | **str** | **Deprecated:** use &#x60;summary&#x60; instead.  A summary of the reasoning performed by the model. This can be useful for debugging and understanding the model&#39;s reasoning process. One of &#x60;auto&#x60;, &#x60;concise&#x60;, or &#x60;detailed&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.reasoning import Reasoning

# TODO update the JSON string below
json = "{}"
# create an instance of Reasoning from a JSON string
reasoning_instance = Reasoning.from_json(json)
# print the JSON string representation of the object
print(Reasoning.to_json())

# convert the object into a dict
reasoning_dict = reasoning_instance.to_dict()
# create an instance of Reasoning from a dict
reasoning_from_dict = Reasoning.from_dict(reasoning_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


