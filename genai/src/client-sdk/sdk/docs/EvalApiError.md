# EvalApiError

An object representing an error response from the Eval API. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**code** | **str** | The error code. | 
**message** | **str** | The error message. | 

## Example

```python
from openapi_client.models.eval_api_error import EvalApiError

# TODO update the JSON string below
json = "{}"
# create an instance of EvalApiError from a JSON string
eval_api_error_instance = EvalApiError.from_json(json)
# print the JSON string representation of the object
print(EvalApiError.to_json())

# convert the object into a dict
eval_api_error_dict = eval_api_error_instance.to_dict()
# create an instance of EvalApiError from a dict
eval_api_error_from_dict = EvalApiError.from_dict(eval_api_error_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


